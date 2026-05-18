/*
 * huffman.cpp - JPEG Huffman stream validator with checkpoint/restore
 *
 * Validates JPEG entropy-coded data by decoding the Huffman bitstream
 * and detecting two error signals:
 *   Signal 1: Invalid Huffman code lookup (no code matches at any length 1..16)
 *   Signal 2: Quantization array overflow (zig-zag position > 64)
 *
 * Does NOT reconstruct pixel values - only validates stream structure.
 * This is the core fragmentation detector for sdrecov.
 *
 * Reference: van der Meer et al. 2024 (jpeg-fragments), ITU-T T.81 Annex F,
 *            docs/jpeg_spec_extract.md, docs/jpeg_fragments_analysis.md
 */
#include "sdrecov.h"
#include <cstring>

/* Forward declare BitStream (defined in bitstream.cpp) */
struct BitStream {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    uint64_t       buf;
    int            avail;
    bool           hit_marker;
    uint8_t        marker_byte;
};

extern void   bs_init(BitStream &bs, const uint8_t *data, size_t len);
extern int    bs_peek(BitStream &bs, int nbits);
extern void   bs_skip(BitStream &bs, int nbits);
extern int    bs_read(BitStream &bs, int nbits);
extern size_t bs_byte_offset(const BitStream &bs);
extern void   bs_align(BitStream &bs);
extern bool   bs_consume_restart(BitStream &bs, int expected_rst);

/*
 * Decode one Huffman symbol using the fast lookup table.
 * Returns >= 0: symbol value
 *         -1: DECODE_INVALID (no valid code at any length 1..16)
 *         -2: DECODE_EOF (ran out of data)
 *         -3: DECODE_MARKER (hit a JPEG marker)
 */
constexpr int DECODE_INVALID = -1;
constexpr int DECODE_EOF     = -2;
constexpr int DECODE_MARKER  = -3;

static int huff_decode_symbol(BitStream &bs, const HuffTable &ht)
{
    /* Safety: reject uninitialized tables (max_code_length == 0 means no codes) */
    if (ht.num_symbols == 0 && ht.max_code_length == 0)
        return -1;

    /* Fast path: 8-bit lookahead */
    int peek = bs_peek(bs, 8);

    if (peek < 0) {
        /*
         * Can't fill to 8 bits (hit marker or EOF). But we may still have
         * some valid bits in the buffer - this happens at the end of a
         * restart interval where bs_fill encounters the RST marker but the
         * last Huffman code is shorter than 8 bits.
         *
         * Try the fast lookup with available bits, zero-padded to 8.
         * The lookup table handles all suffix variations, so zero-padding
         * the low bits is fine for codes shorter than the available bits.
         */
        if (bs.avail > 0) {
            int partial = (int)(bs.buf >> (64 - 8));
            int nbits = ht.look_nbits[partial];
            if (nbits > 0 && nbits <= bs.avail) {
                bs_skip(bs, nbits);
                return ht.look_sym[partial];
            }
        }
        return peek;  /* propagate -2 (EOF) or -3 (MARKER) */
    }

    int nbits = ht.look_nbits[peek];
    if (nbits > 0) {
        bs_skip(bs, nbits);
        return ht.look_sym[peek];
    }

    /*
     * Slow path: code is longer than 8 bits.
     * We already have the top 8 bits in 'peek'. Extend bit-by-bit
     * from length 9 up to 16, checking maxcode at each length.
     * DON'T consume bits until we find the match.
     */
    int code = peek;
    bs_skip(bs, 8);

    for (int l = 9; l <= 16; l++) {
        int next_bit = bs_read(bs, 1);
        if (next_bit < 0) return next_bit;  /* propagate -2 or -3 */
        code = (code << 1) | next_bit;

        if (ht.maxcode[l] >= 0 && code <= ht.maxcode[l]) {
            int idx = code + ht.valoffset[l];
            if (idx < 0 || idx >= 256) return -1;
            return ht.huffval[idx];
        }
    }

    /* No valid code found at any length 1..16 => Signal 1 */
    return -1;
}

/* Map huff_decode_symbol return to HuffError */
static inline HuffError decode_err(int sym, HuffError code_err)
{
    if (sym == DECODE_EOF)    return HUFF_ERR_EOF;
    if (sym == DECODE_MARKER) return HUFF_ERR_MARKER;
    return code_err;  /* DECODE_INVALID -> HUFF_ERR_DC or HUFF_ERR_AC */
}

/* Map bs_read return to HuffError for extra bits */
static inline HuffError read_err(int val)
{
    if (val == DECODE_EOF)    return HUFF_ERR_EOF;
    if (val == DECODE_MARKER) return HUFF_ERR_MARKER;
    return HUFF_ERR_EOF;  /* shouldn't happen, safe fallback */
}

/*
 * Validate one 8x8 block (DC + AC coefficients).
 * Returns HUFF_OK on success, or an error code.
 *
 * This is the core fragmentation detection logic.
 * Reference: jpeg-fragments JpegBaseline.java:129-173
 */
static HuffError validate_block(BitStream &bs,
                                 const HuffTable &dc_table,
                                 const HuffTable &ac_table,
                                 int32_t &dc_pred)
{
    /* ---- DC coefficient ---- */
    int dc_sym = huff_decode_symbol(bs, dc_table);
    if (dc_sym < 0) return decode_err(dc_sym, HUFF_ERR_DC);

    /* dc_sym = SSSS = category (number of additional bits) */
    if (dc_sym > 0) {
        int extra = bs_read(bs, dc_sym);
        if (extra < 0) return read_err(extra);
        /* Reconstruct DC difference (not needed for validation, but update predictor) */
        int diff;
        if (extra >= (1 << (dc_sym - 1)))
            diff = extra;
        else
            diff = extra - (1 << dc_sym) + 1;
        dc_pred += diff;
    }
    /* SSSS=0 means DIFF=0, predictor unchanged */

    /* ---- AC coefficients (indices 1..63) ---- */
    int qa_pos = 1; /* quantization array position, starts at 1 after DC */

    while (qa_pos < 64) {
        int ac_sym = huff_decode_symbol(bs, ac_table);
        if (ac_sym < 0) return decode_err(ac_sym, HUFF_ERR_AC);

        if (ac_sym == 0x00) {
            break;
        }

        int rrrr = (ac_sym >> 4) & 0x0F;
        int ssss = ac_sym & 0x0F;

        if (ac_sym == 0xF0) {
            qa_pos += 16;
        } else {
            qa_pos += rrrr;
            qa_pos++;

            if (qa_pos > 64) return HUFF_ERR_QA;

            if (ssss > 0) {
                int extra = bs_read(bs, ssss);
                if (extra < 0) return read_err(extra);
            }
        }
    }

    return HUFF_OK;
}

/* ---- Progressive JPEG block validators ---- */

/*
 * DC-only first scan (Ss=0, Se=0, Ah=0):
 * Decode exactly 1 DC symbol per block. No AC coefficients.
 */
static HuffError validate_block_prog_dc_first(BitStream &bs,
                                               const HuffTable &dc_table,
                                               int32_t &dc_pred)
{
    int dc_sym = huff_decode_symbol(bs, dc_table);
    if (dc_sym < 0) return decode_err(dc_sym, HUFF_ERR_DC);

    if (dc_sym > 0) {
        int extra = bs_read(bs, dc_sym);
        if (extra < 0) return read_err(extra);
        int diff;
        if (extra >= (1 << (dc_sym - 1)))
            diff = extra;
        else
            diff = extra - (1 << dc_sym) + 1;
        dc_pred += diff;
    }
    return HUFF_OK;
}

/*
 * DC refinement scan (Ss=0, Se=0, Ah!=0):
 * Read exactly 1 bit per block (next bit of precision).
 */
static HuffError validate_block_prog_dc_refine(BitStream &bs)
{
    int bit = bs_read(bs, 1);
    if (bit < 0) return read_err(bit);
    return HUFF_OK;
}

/*
 * AC first scan (Ss>0, Ah=0):
 * Decode AC coefficients in spectral range [Ss..Se].
 * Handles EOBn runs (end-of-band for multiple blocks).
 */
static HuffError validate_block_prog_ac_first(BitStream &bs,
                                               const HuffTable &ac_table,
                                               uint8_t ss, uint8_t se,
                                               uint16_t &eob_run)
{
    /* If in an EOB run, just decrement and return */
    if (eob_run > 0) {
        eob_run--;
        return HUFF_OK;
    }

    int qa_pos = ss;

    while (qa_pos <= se) {
        int ac_sym = huff_decode_symbol(bs, ac_table);
        if (ac_sym < 0) return decode_err(ac_sym, HUFF_ERR_AC);

        int rrrr = (ac_sym >> 4) & 0x0F;
        int ssss = ac_sym & 0x0F;

        if (ssss == 0) {
            if (rrrr == 15) {
                qa_pos += 16;
            } else {
                int eob_extra = 0;
                if (rrrr > 0) {
                    eob_extra = bs_read(bs, rrrr);
                    if (eob_extra < 0) return read_err(eob_extra);
                }
                eob_run = (1 << rrrr) + eob_extra - 1; /* minus 1: current block done */
                return HUFF_OK;
            }
        } else {
            qa_pos += rrrr;  /* skip rrrr zeros */
            qa_pos++;         /* this coefficient */

            if (qa_pos > se + 1) return HUFF_ERR_QA;

            /* Read coefficient magnitude bits */
            int extra = bs_read(bs, ssss);
            if (extra < 0) return read_err(extra);
        }
    }

    return HUFF_OK;
}

/*
 * AC refinement scan (Ss>0, Ah!=0):
 * Simplified validation: decode Huffman symbols for structure checking
 * but don't track exact refinement bits (would need coefficient history).
 * SSSS must be 0 or 1 in AC refinement.
 */
static HuffError validate_block_prog_ac_refine(BitStream &bs,
                                                const HuffTable &ac_table,
                                                uint8_t ss, uint8_t se,
                                                uint16_t &eob_run)
{
    if (eob_run > 0) {
        eob_run--;
        return HUFF_OK;
    }

    int qa_pos = ss;

    while (qa_pos <= se) {
        int ac_sym = huff_decode_symbol(bs, ac_table);
        if (ac_sym < 0) return decode_err(ac_sym, HUFF_ERR_AC);

        int rrrr = (ac_sym >> 4) & 0x0F;
        int ssss = ac_sym & 0x0F;

        if (ssss == 0) {
            if (rrrr == 15) {
                qa_pos += 16;
            } else {
                /* EOBn */
                int eob_extra = 0;
                if (rrrr > 0) {
                    eob_extra = bs_read(bs, rrrr);
                    if (eob_extra < 0) return read_err(eob_extra);
                }
                eob_run = (1 << rrrr) + eob_extra - 1;
                return HUFF_OK;
            }
        } else if (ssss == 1) {
            int sign = bs_read(bs, 1);
            if (sign < 0) return read_err(sign);
            qa_pos += rrrr;
            qa_pos++;
            if (qa_pos > se + 1) return HUFF_ERR_QA;
        } else {
            /* SSSS must be 0 or 1 in AC refinement */
            return HUFF_ERR_AC;
        }
    }

    return HUFF_OK;
}

/*
 * Progressive block dispatcher: routes to the correct validator
 * based on scan parameters (spectral selection + successive approximation).
 */
static HuffError validate_block_progressive(BitStream &bs,
                                             const HuffTable &dc_table,
                                             const HuffTable &ac_table,
                                             int32_t &dc_pred,
                                             HuffCheckpoint &state)
{
    bool is_dc = (state.scan_ss == 0);
    bool is_first = (state.scan_ah == 0);

    if (is_dc && is_first)
        return validate_block_prog_dc_first(bs, dc_table, dc_pred);
    if (is_dc && !is_first)
        return validate_block_prog_dc_refine(bs);
    if (!is_dc && is_first)
        return validate_block_prog_ac_first(bs, ac_table, state.scan_ss, state.scan_se, state.eob_run);
    /* !is_dc && !is_first */
    return validate_block_prog_ac_refine(bs, ac_table, state.scan_ss, state.scan_se, state.eob_run);
}

/*
 * Validate a cluster of JPEG entropy-coded data.
 *
 * Continues from the decoder state in 'state', validates as many MCUs
 * as the data allows, updates 'state' with final position.
 *
 * Returns HuffResult with pass/fail, byte offset of error, MCU count.
 */
HuffResult huff_validate_cluster(const uint8_t *data, size_t len,
                                  const McuConfig &cfg,
                                  const HuffTable dc_tables[],
                                  const HuffTable ac_tables[],
                                  HuffCheckpoint &state)
{
    HuffResult result = {};

    BitStream bs;
    bs_init(bs, data, len);

    /* Restore bit buffer state from checkpoint */
    bs.buf   = state.bit_buffer;
    bs.avail = state.bits_left;
    bs.pos   = state.byte_pos;

    while (state.mcu_count < cfg.total_mcus) {
        /* Check for restart marker */
        if (cfg.restart_interval > 0 && state.mcus_to_restart == 0
            && state.mcu_count > 0) {
            int expected = state.rst_counter % 8;
            if (!bs_consume_restart(bs, expected)) {
                result.offset = bs_byte_offset(bs);
                result.error_type = HUFF_ERR_RST;
                result.mcu_count = state.mcu_count;
                return result;
            }
            /* Reset DC predictors */
            for (int i = 0; i < MAX_COMPONENTS; i++)
                state.dc_pred[i] = 0;
            state.rst_counter++;
            state.mcus_to_restart = cfg.restart_interval;
        }

        /* Validate all blocks in this MCU */
        for (int b = state.block_index; b < cfg.blocks_per_mcu; b++) {
            int comp = cfg.block_comp[b];
            int dc_tbl = cfg.block_dc_tbl[b];
            int ac_tbl = cfg.block_ac_tbl[b];

            HuffError err;
            if (cfg.jpeg_mode == JPEG_PROGRESSIVE) {
                err = validate_block_progressive(bs,
                                                  dc_tables[dc_tbl],
                                                  ac_tables[ac_tbl],
                                                  state.dc_pred[comp],
                                                  state);
            } else {
                err = validate_block(bs,
                                      dc_tables[dc_tbl],
                                      ac_tables[ac_tbl],
                                      state.dc_pred[comp]);
            }

            if (err != HUFF_OK) {
                if (err == HUFF_ERR_EOF) {
                    /* Ran out of data. Normal at cluster boundaries.
                     * Save state for continuation with next cluster. */
                    state.bit_buffer = bs.buf;
                    state.bits_left  = bs.avail;
                    state.byte_pos   = bs.pos;
                    state.block_index = b;
                    result.passed = true;
                    result.error_type = HUFF_ERR_EOF;
                    result.mcu_count = state.mcu_count;
                    return result;
                }

                if (err == HUFF_ERR_MARKER && bs.hit_marker) {
                    /* Hit a JPEG marker during block decoding. */

                    /* Progressive: SOS/EOI = scan boundary */
                    if (cfg.jpeg_mode == JPEG_PROGRESSIVE &&
                        (bs.marker_byte == 0xDA || bs.marker_byte == 0xD9)) {
                        state.bit_buffer = 0;
                        state.bits_left  = 0;
                        state.byte_pos   = bs.pos;
                        state.block_index = 0;
                        result.passed = true;
                        result.error_type = HUFF_ERR_MARKER;
                        result.mcu_count = state.mcu_count;
                        return result;
                    }

                    /* RST marker hit during last MCU of restart interval.
                     * This happens when bs_fill encounters the RST marker
                     * while trying to pre-fill the buffer and the Huffman
                     * code doesn't fit in the remaining bits. Treat as
                     * successful end of interval - bs_consume_restart will
                     * handle the marker on the next iteration. */
                    if (bs.marker_byte >= 0xD0 && bs.marker_byte <= 0xD7 &&
                        cfg.restart_interval > 0 && state.mcus_to_restart <= 1) {
                        /* Pretend this MCU completed. The decoder was at the
                         * very end of the interval data. */
                        state.block_index = 0;
                        state.mcu_count++;
                        state.mcus_to_restart = 0;
                        state.bit_buffer = bs.buf;
                        state.bits_left  = bs.avail;
                        state.byte_pos   = bs.pos;
                        /* Continue the main loop - it will hit the RST check */
                        goto continue_after_mcu;
                    }
                }

                /* HUFF_ERR_DC, HUFF_ERR_AC, HUFF_ERR_QA = real errors (bad data) */
                result.offset = bs_byte_offset(bs);
                result.error_type = err;
                result.mcu_count = state.mcu_count;
                return result;
            }
        }

        /* MCU complete */
        state.block_index = 0;
        state.mcu_count++;
        if (cfg.restart_interval > 0)
            state.mcus_to_restart--;

    continue_after_mcu:
        /* Check for SOS marker (progressive scan boundary) or EOI */
        if (bs.hit_marker && bs.marker_byte == 0xDA &&
            cfg.jpeg_mode == JPEG_PROGRESSIVE) {
            /* Scan complete, next SOS found. For chain validation purposes,
             * we've successfully decoded all MCUs of this scan. Reset for
             * the next scan: MCU count restarts, DC predictors reset. */
            state.bit_buffer = 0;
            state.bits_left  = 0;
            state.byte_pos   = bs.pos;
            result.passed = true;
            result.mcu_count = state.mcu_count;
            return result;
        }
        if (bs.hit_marker && bs.marker_byte == 0xD9) {
            state.bit_buffer = 0;
            state.bits_left  = 0;
            state.byte_pos   = bs.pos;
            result.passed = true;
            result.mcu_count = state.mcu_count;
            return result;
        }
    }

    /* All MCUs validated */
    state.bit_buffer = bs.buf;
    state.bits_left  = bs.avail;
    state.byte_pos   = bs.pos;
    result.passed = true;
    result.mcu_count = state.mcu_count;
    return result;
}

/*
 * Validate exactly ONE MCU from the data buffer.
 * Used for boundary checking: decode the first MCU of a candidate cluster
 * to get the DC predictor values after that single MCU, enabling precise
 * boundary discontinuity detection.
 */
HuffResult huff_validate_one_mcu(const uint8_t *data, size_t len,
                                  const McuConfig &cfg,
                                  const HuffTable dc_tables[],
                                  const HuffTable ac_tables[],
                                  HuffCheckpoint &state)
{
    HuffResult result = {};

    BitStream bs;
    bs_init(bs, data, len);
    bs.buf   = state.bit_buffer;
    bs.avail = state.bits_left;
    bs.pos   = state.byte_pos;

    /* Handle restart marker if due */
    if (cfg.restart_interval > 0 && state.mcus_to_restart == 0
        && state.mcu_count > 0) {
        int expected = state.rst_counter % 8;
        if (!bs_consume_restart(bs, expected)) {
            result.error_type = HUFF_ERR_RST;
            return result;
        }
        for (int i = 0; i < MAX_COMPONENTS; i++)
            state.dc_pred[i] = 0;
        state.rst_counter++;
        state.mcus_to_restart = cfg.restart_interval;
    }

    /* Validate exactly one MCU */
    for (int b = state.block_index; b < cfg.blocks_per_mcu; b++) {
        int comp = cfg.block_comp[b];
        int dc_tbl = cfg.block_dc_tbl[b];
        int ac_tbl = cfg.block_ac_tbl[b];

        HuffError err;
        if (cfg.jpeg_mode == JPEG_PROGRESSIVE) {
            err = validate_block_progressive(bs,
                                              dc_tables[dc_tbl],
                                              ac_tables[ac_tbl],
                                              state.dc_pred[comp],
                                              state);
        } else {
            err = validate_block(bs,
                                  dc_tables[dc_tbl],
                                  ac_tables[ac_tbl],
                                  state.dc_pred[comp]);
        }
        if (err != HUFF_OK) {
            state.bit_buffer = bs.buf;
            state.bits_left  = bs.avail;
            state.byte_pos   = bs.pos;

            if (err == HUFF_ERR_EOF) {
                result.passed = true;
                result.error_type = HUFF_ERR_EOF;
                result.mcu_count = state.mcu_count;
            } else if (err == HUFF_ERR_MARKER) {
                result.passed = true;
                result.error_type = HUFF_ERR_MARKER;
                result.mcu_count = state.mcu_count;
            } else {
                result.error_type = err;
                result.offset = bs_byte_offset(bs);
            }
            return result;
        }
    }

    /* One MCU complete */
    state.block_index = 0;
    state.mcu_count++;
    if (cfg.restart_interval > 0)
        state.mcus_to_restart--;

    state.bit_buffer = bs.buf;
    state.bits_left  = bs.avail;
    state.byte_pos   = bs.pos;

    result.passed = true;
    result.mcu_count = state.mcu_count;
    return result;
}

/*
 * Corruption-tolerant validation: when the decoder hits an error, skip
 * ahead and try to resynchronize. Huffman coding self-synchronizes within
 * 2-30 MCUs from an arbitrary starting position (Tang et al., 2015).
 *
 * Returns the total number of validated MCUs across all segments
 * (before and after each resync point).
 */
uint32_t huff_validate_tolerant(const uint8_t *data, size_t len,
                                 const McuConfig &cfg,
                                 const HuffTable dc_tables[],
                                 const HuffTable ac_tables[],
                                 int max_resync)
{
    uint32_t total_mcus = 0;
    int resyncs = 0;

    HuffCheckpoint state = {};
    if (cfg.restart_interval > 0)
        state.mcus_to_restart = cfg.restart_interval;

    /* First pass: normal validation */
    HuffResult r = huff_validate_cluster(data, len, cfg, dc_tables, ac_tables, state);
    total_mcus = r.mcu_count;

    if (r.passed || r.error_type == HUFF_ERR_EOF)
        return total_mcus;

    /* Try resynchronizing after each error */
    size_t skip_pos = r.offset + 1;

    while (resyncs < max_resync && skip_pos + 64 < len) {
        /* Try decoding from skip_pos with a fresh state.
         * Reset DC predictors (unknown after gap). */
        HuffCheckpoint resync_state = {};
        resync_state.byte_pos = skip_pos;
        if (cfg.restart_interval > 0)
            resync_state.mcus_to_restart = cfg.restart_interval;

        HuffResult rr = huff_validate_cluster(
            data, len, cfg, dc_tables, ac_tables, resync_state);

        if (rr.mcu_count > 5) {
            /* Found valid data after the error */
            total_mcus += rr.mcu_count;
        }

        resyncs++;

        if (rr.passed || rr.error_type == HUFF_ERR_EOF)
            break;

        /* Another error - skip past it */
        skip_pos = resync_state.byte_pos + 1;
        if (skip_pos <= r.offset + 1) skip_pos = r.offset + 2;
    }

    return total_mcus;
}
