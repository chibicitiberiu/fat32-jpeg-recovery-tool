/*
 * jpeg_parse.cpp - JPEG marker parser
 *
 * Parses SOF, SOS, DHT, DQT, DRI markers from a JPEG header.
 * Builds HuffTable decode tables and McuConfig from parsed data.
 * Direct byte-level parsing (no Metal DSL).
 *
 * Reference: ITU-T T.81 Annex B, docs/jpeg_spec_extract.md
 */
#include "sdrecov.h"
#include <cstring>
#include <algorithm>

/* Standard Annex K tables (defined in engine.cpp) */
extern HuffTable g_std_dc[2], g_std_ac[2];
extern void build_standard_tables();

/* Feature flag for DHT fallback (set by main before recovery starts) */
bool g_dht_fallback_enabled = true;

static uint16_t rd16(const uint8_t *p) { return (uint16_t(p[0]) << 8) | p[1]; }

/*
 * Build Huffman decode table from BITS[16] and HUFFVAL arrays.
 * Implements ITU-T T.81 Annex C, Figures C.1 and C.2.
 * Also builds 8-bit lookahead table (libjpeg-turbo pattern).
 */
bool huff_table_build(HuffTable &ht, const uint8_t bits[16],
                      const uint8_t *huffval, int num_symbols)
{
    ht = {};
    ht.num_symbols = num_symbols;
    memcpy(ht.huffval, huffval, std::min(num_symbols, 256));

    /* Generate HUFFSIZE: list of code lengths (Figure C.1) */
    int huffsize[257] = {};
    int k = 0;
    for (int i = 0; i < 16; i++) {
        for (int j = 0; j < bits[i]; j++) {
            if (k >= 256) return false;
            huffsize[k++] = i + 1;
        }
    }
    huffsize[k] = 0;
    (void)k; /* lastk not needed for decode tables */

    /* Generate HUFFCODE: canonical code assignment (Figure C.2) */
    int huffcode[257] = {};
    k = 0;
    int code = 0;
    int si = huffsize[0];
    while (huffsize[k] != 0) {
        while (huffsize[k] == si) {
            huffcode[k] = code;
            code++;
            k++;
            if (k >= 257) return false;
        }
        /* Shift for next code length. This naturally exceeds 16 bits
         * after the last used length, which is fine - we won't assign
         * any more codes. Only check while we still have symbols. */
        code <<= 1;
        si++;
    }

    /* Build maxcode/valoffset arrays for fast decode */
    /* maxcode[l] = largest code of length l, or -1 if none */
    /* valoffset[l] = offset to subtract for symbol lookup */
    for (int i = 0; i < 18; i++) {
        ht.maxcode[i] = -1;
        ht.valoffset[i] = 0;
    }

    int p = 0;
    for (int l = 1; l <= 16; l++) {
        if (bits[l - 1] > 0) {
            ht.valoffset[l] = p - huffcode[p];
            p += bits[l - 1];
            ht.maxcode[l] = huffcode[p - 1];
        }
    }
    ht.max_code_length = 0;
    for (int l = 16; l >= 1; l--) {
        if (bits[l - 1] > 0) { ht.max_code_length = l; break; }
    }

    /* Build 8-bit lookahead table */
    /* For codes <= 8 bits, look_nbits[prefix] = code length, look_sym[prefix] = symbol */
    memset(ht.look_nbits, 0, sizeof(ht.look_nbits));

    p = 0;
    for (int l = 1; l <= 8; l++) {
        for (int i = 0; i < bits[l - 1]; i++) {
            int prefix = huffcode[p] << (8 - l);
            int fill = 1 << (8 - l);
            if (prefix < 0 || prefix + fill > 256) {
                /* Corrupt Huffman table - code overflows lookup array */
                return false;
            }
            for (int j = 0; j < fill; j++) {
                ht.look_nbits[prefix + j] = l;
                ht.look_sym[prefix + j] = huffval[p];
            }
            p++;
        }
    }

    return true;
}

/*
 * Parse DHT marker segment (after marker and length).
 * Can contain multiple tables.
 */
static bool parse_dht(const uint8_t *data, int len, JpegTemplate &tmpl)
{
    int pos = 0;
    while (pos < len) {
        if (pos + 17 > len) break; /* truncated, but tables parsed so far may be OK */

        uint8_t tc_th = data[pos++];
        int tc = (tc_th >> 4) & 0x0F;  /* 0=DC, 1=AC */
        int th = tc_th & 0x0F;         /* table ID 0-3 */

        if (th >= MAX_HUFF_TABLES) break; /* corrupt tc_th, stop parsing this segment */

        uint8_t bits[16];
        memcpy(bits, data + pos, 16);
        pos += 16;

        int num_symbols = 0;
        for (int i = 0; i < 16; i++) num_symbols += bits[i];

        HuffTable &ht = (tc == 0) ? tmpl.dc_tables[th] : tmpl.ac_tables[th];

        if (pos + num_symbols > len || num_symbols > 256) {
            if (!g_dht_fallback_enabled) return false;
            /* Segment overflow - substitute standard Annex K table */
            build_standard_tables();
            int std_idx = (th <= 1) ? th : 0;
            ht = (tc == 0) ? g_std_dc[std_idx] : g_std_ac[std_idx];
            break; /* can't parse remaining tables in this segment */
        }

        const uint8_t *huffval = data + pos;
        pos += num_symbols;

        if (!huff_table_build(ht, bits, huffval, num_symbols)) {
            if (!g_dht_fallback_enabled) return false;
            build_standard_tables();
            int std_idx = (th <= 1) ? th : 0;
            ht = (tc == 0) ? g_std_dc[std_idx] : g_std_ac[std_idx];
        }
    }
    return true;
}

/*
 * Parse SOF marker (baseline SOF0 or progressive SOF2).
 */
static bool parse_sof(const uint8_t *data, int len, JpegTemplate &tmpl, McuConfig &cfg)
{
    if (len < 6) return false;

    /* uint8_t P = data[0]; */ /* sample precision, always 8 for baseline */
    cfg.image_height = rd16(data + 1);
    cfg.image_width  = rd16(data + 3);
    cfg.num_components = data[5];

    tmpl.height = cfg.image_height;
    tmpl.width  = cfg.image_width;

    if (cfg.num_components < 1 || cfg.num_components > MAX_COMPONENTS)
        return false;
    if (len < 6 + cfg.num_components * 3)
        return false;

    /* Parse component specs */
    uint8_t h_samp[MAX_COMPONENTS] = {}, v_samp[MAX_COMPONENTS] = {};

    for (int i = 0; i < cfg.num_components; i++) {
        int off = 6 + i * 3;
        /* comp_id = data[off]; not needed for validation */
        h_samp[i] = (data[off + 1] >> 4) & 0x0F;
        v_samp[i] = data[off + 1] & 0x0F;
        /* tq = data[off + 2]; quantization table selector */
    }

    /* Compute MCU geometry */
    uint8_t hmax = 1, vmax = 1;
    for (int i = 0; i < cfg.num_components; i++) {
        if (h_samp[i] > hmax) hmax = h_samp[i];
        if (v_samp[i] > vmax) vmax = v_samp[i];
    }
    tmpl.subsampling = (hmax << 4) | vmax;

    uint16_t mcu_w_px = 8 * hmax;
    uint16_t mcu_h_px = 8 * vmax;
    cfg.mcu_width  = (cfg.image_width + mcu_w_px - 1) / mcu_w_px;
    cfg.mcu_height = (cfg.image_height + mcu_h_px - 1) / mcu_h_px;
    cfg.total_mcus = (uint32_t)cfg.mcu_width * cfg.mcu_height;

    /* Build block map: for each component, Hi*Vi blocks */
    cfg.blocks_per_mcu = 0;
    for (int c = 0; c < cfg.num_components; c++) {
        int nblocks = h_samp[c] * v_samp[c];
        for (int b = 0; b < nblocks; b++) {
            if (cfg.blocks_per_mcu >= MAX_BLOCKS_PER_MCU) return false;
            cfg.block_comp[cfg.blocks_per_mcu] = c;
            cfg.blocks_per_mcu++;
        }
    }

    return true;
}

/*
 * Parse SOS marker (scan header).
 * Sets Huffman table selectors per block in McuConfig.
 */
static bool parse_sos(const uint8_t *data, int len, JpegTemplate &/*tmpl*/, McuConfig &cfg)
{
    if (len < 1) return false;
    uint8_t ns = data[0]; /* number of components in scan */
    if (len < 1 + ns * 2 + 3) return false;

    /* For each component in scan, set DC/AC table selectors */
    /* We need to map scan component index back to block positions */
    int block_idx = 0;
    for (int j = 0; j < ns; j++) {
        /* uint8_t cs = data[1 + j * 2]; */ /* component selector */
        uint8_t td_ta = data[1 + j * 2 + 1];
        uint8_t td = (td_ta >> 4) & 0x0F;
        uint8_t ta = td_ta & 0x0F;
        /* Reject corrupted table indices that would cause OOB access */
        if (td >= MAX_HUFF_TABLES || ta >= MAX_HUFF_TABLES)
            return false;

        /* Find how many blocks this component has (from SOF parsing) */
        int comp_blocks = 0;
        for (int b = block_idx; b < cfg.blocks_per_mcu; b++) {
            if (cfg.block_comp[b] == j)
                comp_blocks++;
            else if (comp_blocks > 0)
                break;
        }

        for (int b = 0; b < comp_blocks && block_idx < cfg.blocks_per_mcu; b++) {
            cfg.block_dc_tbl[block_idx] = td;
            cfg.block_ac_tbl[block_idx] = ta;
            block_idx++;
        }
    }

    /* Spectral selection and successive approximation */
    int ss_offset = 1 + ns * 2;
    uint8_t ss = data[ss_offset];
    uint8_t se = data[ss_offset + 1];
    uint8_t ah_al = data[ss_offset + 2];
    uint8_t ah = (ah_al >> 4) & 0x0F;
    uint8_t al = ah_al & 0x0F;

    /* Only set mode from SOS if SOF didn't already determine it.
     * SOF2 = progressive regardless of first scan's spectral range.
     * Some progressive JPEGs have Ss=0,Se=63 in first scan (full range). */
    if (cfg.jpeg_mode == JPEG_UNKNOWN) {
        if (ss == 0 && se == 63 && ah == 0 && al == 0)
            cfg.jpeg_mode = JPEG_BASELINE;
        else
            cfg.jpeg_mode = JPEG_PROGRESSIVE;
    }

    /* Store scan parameters */
    if (cfg.num_scans < 32) {
        ScanConfig &sc = cfg.scans[cfg.num_scans];
        sc.num_components = ns;
        sc.ss = ss;
        sc.se = se;
        sc.ah = ah;
        sc.al = al;
        for (int j = 0; j < ns && j < MAX_COMPONENTS; j++) {
            sc.comp_index[j] = j;
            sc.dc_tbl[j] = cfg.block_dc_tbl[j];
            sc.ac_tbl[j] = cfg.block_ac_tbl[j];
        }
        cfg.num_scans++;
    }

    return true;
}

/*
 * Parse DQT marker. Extract quantization tables for DQT fingerprinting.
 */
static bool parse_dqt(const uint8_t *data, int len, JpegTemplate &tmpl)
{
    int pos = 0;
    while (pos < len) {
        if (pos + 1 > len) return false;
        uint8_t pq_tq = data[pos++];
        int pq = (pq_tq >> 4) & 0x0F;  /* precision: 0=8bit, 1=16bit */
        int tq = pq_tq & 0x0F;          /* table id */

        int table_size = (pq == 0) ? 64 : 128;
        if (pos + table_size > len) return false;

        if (tq == 0)
            memcpy(tmpl.dqt_luma, data + pos, std::min(table_size, 64));
        else if (tq == 1)
            memcpy(tmpl.dqt_chroma, data + pos, std::min(table_size, 64));

        pos += table_size;
    }
    return true;
}

/*
 * Extract EXIF thumbnail from APP1 segment.
 * EXIF thumbnails are embedded as a complete JPEG within the TIFF IFD1.
 * We look for FFD8 within the APP1 data (after the EXIF header).
 * Returns offset and size relative to start of full JPEG data, or 0 if not found.
 */
bool jpeg_extract_thumbnail(const uint8_t *data, size_t len,
                             uint32_t &thumb_offset, uint32_t &thumb_size)
{
    thumb_offset = 0;
    thumb_size = 0;

    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8)
        return false;

    size_t pos = 2;
    while (pos + 3 < len) {
        if (data[pos] != 0xFF) { pos++; continue; }
        while (pos + 1 < len && data[pos + 1] == 0xFF) pos++;
        if (pos + 1 >= len) break;

        uint8_t marker = data[pos + 1];
        pos += 2;

        if (marker == 0xD9 || marker == 0xDA) break; /* EOI or SOS = past headers */
        if (pos + 2 > len) break;

        uint16_t seg_len = (uint16_t(data[pos]) << 8) | data[pos + 1];
        if (pos + seg_len > len) break;

        if (marker == 0xE1) { /* APP1 = EXIF */
            /* Search for embedded JPEG (FFD8FF) within this segment */
            const uint8_t *seg = data + pos + 2;
            int seg_body = seg_len - 2;

            for (int i = 0; i + 2 < seg_body; i++) {
                if (seg[i] == 0xFF && seg[i + 1] == 0xD8 && seg[i + 2] == 0xFF) {
                    /* Found embedded JPEG thumbnail */
                    uint32_t abs_offset = (uint32_t)(pos + 2 + i);

                    /* Find its end (FFD9) */
                    for (int j = i + 3; j + 1 < seg_body; j++) {
                        if (seg[j] == 0xFF && seg[j + 1] == 0xD9) {
                            thumb_offset = abs_offset;
                            thumb_size = (pos + 2 + j + 2) - abs_offset;
                            return true;
                        }
                    }
                    /* No EOI found, use rest of segment */
                    thumb_offset = abs_offset;
                    thumb_size = (pos + seg_len) - abs_offset;
                    return true;
                }
            }
        }

        pos += seg_len;
    }
    return false;
}

/*
 * Compute DQT fingerprint distance (Sum of Absolute Differences).
 * Lower = more similar. 0 = identical tables.
 */
int dqt_distance(const uint8_t a[64], const uint8_t b[64])
{
    int sad = 0;
    for (int i = 0; i < 64; i++)
        sad += std::abs((int)a[i] - (int)b[i]);
    return sad;
}

/*
 * Parse a complete JPEG header (SOI through SOS).
 * Extracts all tables, geometry, and builds McuConfig.
 */
bool jpeg_parse_header(const uint8_t *data, size_t len, JpegTemplate &tmpl)
{
    tmpl = {};
    auto &cfg = tmpl.mcu_config;

    if (len < 2 || data[0] != 0xFF || data[1] != 0xD8)
        return false;

    /* Store raw header bytes */
    size_t pos = 2;
    bool got_sof = false, got_sos = false;

    while (pos + 1 < len) {
        if (data[pos] != 0xFF) { pos++; continue; }

        /* Skip fill bytes */
        while (pos + 1 < len && data[pos + 1] == 0xFF) pos++;
        if (pos + 1 >= len) break;

        uint8_t marker = data[pos + 1];
        pos += 2;

        /* Stand-alone markers */
        if (marker == 0xD8) continue; /* SOI */
        if (marker == 0xD9) break;    /* EOI */

        /* Markers with length */
        if (pos + 2 > len) break;
        uint16_t seg_len = rd16(data + pos);
        if (pos + seg_len > len) break;

        const uint8_t *seg_data = data + pos + 2;
        int seg_body = seg_len - 2;

        switch (marker) {
        case 0xC0: /* SOF0 - baseline */
        case 0xC2: /* SOF2 - progressive */
            if (!parse_sof(seg_data, seg_body, tmpl, cfg)) return false;
            cfg.jpeg_mode = (marker == 0xC0) ? JPEG_BASELINE : JPEG_PROGRESSIVE;
            got_sof = true;
            break;

        case 0xC4: /* DHT */
            if (!parse_dht(seg_data, seg_body, tmpl)) return false;
            break;

        case 0xDB: /* DQT */
            parse_dqt(seg_data, seg_body, tmpl);
            break;

        case 0xDD: /* DRI */
            if (seg_body >= 2) {
                cfg.restart_interval = rd16(seg_data);
                tmpl.restart_interval = cfg.restart_interval;
            }
            break;

        case 0xDA: /* SOS - start of scan */
            if (!got_sof) return false;
            if (!parse_sos(seg_data, seg_body, tmpl, cfg)) return false;
            got_sos = true;
            /* Store everything up to and including SOS header as template */
            tmpl.header_bytes.assign(data, data + pos + seg_len);
            goto done;

        default:
            /* APP0-APPn, COM, etc. - skip */
            break;
        }

        pos += seg_len;
    }

done:
    return got_sof && got_sos;
}

std::vector<uint8_t> inject_dri(const std::vector<uint8_t> &header_bytes,
                                 uint16_t restart_interval)
{
    /* Find SOS marker (FF DA) in the header and insert DRI (FF DD) before it.
     * DRI segment: FF DD 00 04 [interval_hi] [interval_lo] */
    std::vector<uint8_t> result;
    result.reserve(header_bytes.size() + 6);

    const uint8_t *data = header_bytes.data();
    size_t len = header_bytes.size();
    size_t sos_pos = 0;

    /* Scan for SOS marker */
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] == 0xFF && data[i+1] == 0xDA) {
            sos_pos = i;
            break;
        }
    }

    if (sos_pos == 0) {
        /* No SOS found, return original */
        return header_bytes;
    }

    /* Copy everything before SOS */
    result.insert(result.end(), data, data + sos_pos);

    /* Insert DRI marker */
    result.push_back(0xFF);
    result.push_back(0xDD);
    result.push_back(0x00);
    result.push_back(0x04);
    result.push_back((restart_interval >> 8) & 0xFF);
    result.push_back(restart_interval & 0xFF);

    /* Copy SOS and everything after */
    result.insert(result.end(), data + sos_pos, data + len);

    return result;
}
