/*
 * jpeg_targeted.cpp - JPEG-aware corruption pass
 *
 * Unlike the generic bitflip pass, this targets specific JPEG structures:
 * - DHT (Huffman table) bit flips
 * - DQT (quantization table) bit flips
 * - SOF (frame header) dimension corruption
 * - Byte-stuffing (FF 00) corruption to create accidental markers
 * - RST marker corruption
 * - Partial cluster zeroing (first/last 512 bytes)
 *
 * Operates after ground truth recording, using the manifest to locate
 * JPEG files and their cluster chains on disk.
 */
#include "corrsim.h"
#include <cstring>

/*
 * Find JPEG marker segments in a data buffer.
 * Returns the offset of the marker (FF xx) or -1 if not found.
 * seg_start/seg_len point to the segment payload (after length field).
 */
struct MarkerInfo {
    size_t marker_offset;  /* position of FF xx */
    size_t payload_offset; /* position of first payload byte (after length) */
    size_t payload_len;    /* payload length */
    uint8_t marker_type;   /* the xx byte */
};

static std::vector<MarkerInfo> find_markers(const uint8_t *data, size_t len,
                                             uint8_t target_marker)
{
    std::vector<MarkerInfo> results;
    size_t pos = 0;

    while (pos + 3 < len) {
        if (data[pos] != 0xFF) { pos++; continue; }
        uint8_t m = data[pos + 1];
        if (m == 0xD8) { pos += 2; continue; } /* SOI */
        if (m == 0xD9) break;                   /* EOI */
        if (m == 0x00) { pos += 2; continue; }  /* stuffed FF */

        if (pos + 3 >= len) break;
        uint16_t seg_len = ((uint16_t)data[pos + 2] << 8) | data[pos + 3];
        if (seg_len < 2 || pos + 2 + seg_len > len) break;

        if (m == target_marker || target_marker == 0) {
            MarkerInfo mi;
            mi.marker_offset = pos;
            mi.payload_offset = pos + 4;
            mi.payload_len = seg_len - 2;
            mi.marker_type = m;
            results.push_back(mi);
        }

        if (m == 0xDA) break; /* SOS = start of entropy data */
        pos += 2 + seg_len;
    }
    return results;
}

/*
 * Find FF 00 byte-stuffing pairs in entropy data (after SOS).
 */
static std::vector<size_t> find_stuffing_pairs(const uint8_t *data, size_t len)
{
    std::vector<size_t> positions;

    /* Find SOS marker first */
    size_t sos_pos = 0;
    size_t pos = 0;
    while (pos + 3 < len) {
        if (data[pos] == 0xFF && data[pos + 1] == 0xDA) {
            uint16_t seg_len = ((uint16_t)data[pos + 2] << 8) | data[pos + 3];
            sos_pos = pos + 2 + seg_len;
            break;
        }
        if (data[pos] == 0xFF && data[pos + 1] == 0xD8) { pos += 2; continue; }
        if (data[pos] == 0xFF && data[pos + 1] != 0x00) {
            if (pos + 3 < len) {
                uint16_t seg_len = ((uint16_t)data[pos + 2] << 8) | data[pos + 3];
                pos += 2 + seg_len;
                continue;
            }
        }
        pos++;
    }

    /* Scan entropy data for FF 00 pairs */
    for (size_t i = sos_pos; i + 1 < len; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0x00) {
            positions.push_back(i + 1); /* position of the 00 byte */
        }
    }
    return positions;
}

/*
 * Find RST markers (FF D0-D7) in entropy data.
 */
static std::vector<size_t> find_rst_markers(const uint8_t *data, size_t len)
{
    std::vector<size_t> positions;
    size_t sos_pos = 0;
    size_t pos = 0;
    while (pos + 3 < len) {
        if (data[pos] == 0xFF && data[pos + 1] == 0xDA) {
            uint16_t seg_len = ((uint16_t)data[pos + 2] << 8) | data[pos + 3];
            sos_pos = pos + 2 + seg_len;
            break;
        }
        if (data[pos] == 0xFF && data[pos + 1] == 0xD8) { pos += 2; continue; }
        if (data[pos] == 0xFF && data[pos + 1] != 0x00 && pos + 3 < len) {
            uint16_t seg_len = ((uint16_t)data[pos + 2] << 8) | data[pos + 3];
            pos += 2 + seg_len;
            continue;
        }
        pos++;
    }

    for (size_t i = sos_pos; i + 1 < len; i++) {
        if (data[i] == 0xFF && data[i + 1] >= 0xD0 && data[i + 1] <= 0xD7) {
            positions.push_back(i);
        }
    }
    return positions;
}

/*
 * Flip a random bit in a byte range.
 */
static void flip_random_bit(SimContext &ctx, uint8_t *data, size_t offset, size_t len)
{
    if (len == 0) return;
    size_t byte_idx = offset + rng_int(ctx, 0, (int)len - 1);
    int bit_idx = rng_int(ctx, 0, 7);
    data[byte_idx] ^= (1 << bit_idx);
}

bool corrupt_jpeg_targeted(SimContext &ctx, uint8_t *data, size_t size)
{
    auto &cfg = ctx.cfg.jpeg;
    auto &geo = ctx.truth.geo;
    uint32_t bpc = geo.bytes_per_cluster;

    log_progress(ctx, "  Pass 4: JPEG-targeted corruption");

    int dht_corrupted = 0, dqt_corrupted = 0, sof_corrupted = 0;
    int stuffing_corrupted = 0, rst_corrupted = 0, partial_zeroed = 0, misaligned = 0;
    int sos_corrupted = 0;

    /* Process each file in the ground truth */
    auto process_file = [&](const FileRecord &file) {
        if (file.clusters.empty()) return;
        uint32_t start_cluster = file.clusters[0];

        /* Read the file's header area (first few clusters) */
        size_t header_size = std::min((size_t)bpc * 4, (size_t)file.file_size);
        uint64_t file_offset = fat32_cluster_offset(geo, start_cluster);
        if (file_offset + header_size > size) return;

        uint8_t *file_data = data + file_offset;

        /* Verify it's a JPEG */
        if (file_data[0] != 0xFF || file_data[1] != 0xD8) return;

        /* DHT corruption */
        if (cfg.dht_corrupt_prob > 0 && rng_bernoulli(ctx, cfg.dht_corrupt_prob)) {
            auto markers = find_markers(file_data, header_size, 0xC4);
            for (auto &mi : markers) {
                int flips = rng_int(ctx, 1, cfg.dht_max_flips);
                for (int f = 0; f < flips; f++) {
                    flip_random_bit(ctx, data, file_offset + mi.payload_offset, mi.payload_len);
                }
            }
            if (!markers.empty()) dht_corrupted++;
        }

        /* DQT corruption */
        if (cfg.dqt_corrupt_prob > 0 && rng_bernoulli(ctx, cfg.dqt_corrupt_prob)) {
            auto markers = find_markers(file_data, header_size, 0xDB);
            for (auto &mi : markers) {
                int flips = rng_int(ctx, 1, cfg.dqt_max_flips);
                for (int f = 0; f < flips; f++) {
                    flip_random_bit(ctx, data, file_offset + mi.payload_offset, mi.payload_len);
                }
            }
            if (!markers.empty()) dqt_corrupted++;
        }

        /* SOF corruption (dimensions) */
        if (cfg.sof_corrupt_prob > 0 && rng_bernoulli(ctx, cfg.sof_corrupt_prob)) {
            auto markers = find_markers(file_data, header_size, 0xC0);
            if (markers.empty())
                markers = find_markers(file_data, header_size, 0xC2);
            for (auto &mi : markers) {
                /* SOF payload: precision(1) + height(2) + width(2) + ... */
                if (mi.payload_len >= 5) {
                    /* Flip 1 bit in the height or width field */
                    size_t dim_offset = mi.payload_offset + 1; /* skip precision */
                    flip_random_bit(ctx, data, file_offset + dim_offset, 4);
                }
            }
            if (!markers.empty()) sof_corrupted++;
        }

        /* SOS table index corruption: set Td/Ta to invalid values 4-15.
         * Tests bounds check in jpeg_parse.cpp parse_sos().
         * Real SD card had AC table index 9, which caused OOB array access. */
        if (cfg.sos_corrupt_prob > 0 && rng_bernoulli(ctx, cfg.sos_corrupt_prob)) {
            auto markers = find_markers(file_data, header_size, 0xDA);
            for (auto &mi : markers) {
                if (mi.payload_len < 1) continue;
                if (mi.payload_offset >= header_size) continue;
                uint8_t ns = file_data[mi.payload_offset];
                /* Reasonable Ns is 1-4 components */
                if (ns < 1 || ns > 4) continue;
                if (mi.payload_len < (size_t)(1 + ns * 2)) continue;
                int comp = rng_int(ctx, 0, ns - 1);
                size_t td_ta_off = mi.payload_offset + 1 + comp * 2 + 1;
                /* Bounds check against the disk image buffer */
                if (td_ta_off >= header_size) continue;
                if (file_offset + td_ta_off >= size) continue;
                uint8_t orig = file_data[td_ta_off];
                uint8_t bad_idx = (uint8_t)rng_int(ctx, 4, 15);
                uint8_t corrupted;
                if (rng_bernoulli(ctx, 0.5))
                    corrupted = (bad_idx << 4) | (orig & 0x0F);
                else
                    corrupted = (orig & 0xF0) | bad_idx;
                data[file_offset + td_ta_off] = corrupted;
                sos_corrupted++;
            }
        }

        /* Byte-stuffing corruption (FF 00 -> FF xx) */
        if (cfg.stuffing_corrupt_prob > 0 && rng_bernoulli(ctx, cfg.stuffing_corrupt_prob)) {
            /* Need full file data for entropy scan */
            size_t full_size = std::min((size_t)file.file_size, size - file_offset);
            auto pairs = find_stuffing_pairs(data + file_offset, full_size);
            if (!pairs.empty()) {
                int to_corrupt = std::min(cfg.stuffing_max_flips, (int)pairs.size());
                /* Shuffle and take first N */
                for (int f = 0; f < to_corrupt; f++) {
                    int idx = rng_int(ctx, f, (int)pairs.size() - 1);
                    std::swap(pairs[f], pairs[idx]);
                    /* Flip the 00 byte to something non-zero (creating accidental marker) */
                    size_t abs_pos = file_offset + pairs[f];
                    if (abs_pos < size) {
                        uint8_t new_val = (uint8_t)rng_int(ctx, 1, 0xBF); /* stay in reserved range */
                        data[abs_pos] = new_val;
                    }
                }
                stuffing_corrupted++;
            }
        }

        /* RST marker corruption */
        if (cfg.rst_corrupt_prob > 0 && rng_bernoulli(ctx, cfg.rst_corrupt_prob)) {
            size_t full_size = std::min((size_t)file.file_size, size - file_offset);
            auto rst_pos = find_rst_markers(data + file_offset, full_size);
            if (!rst_pos.empty()) {
                /* Corrupt 1-3 RST markers */
                int to_corrupt = std::min(3, (int)rst_pos.size());
                for (int f = 0; f < to_corrupt; f++) {
                    int idx = rng_int(ctx, f, (int)rst_pos.size() - 1);
                    std::swap(rst_pos[f], rst_pos[idx]);
                    size_t abs_pos = file_offset + rst_pos[f];
                    if (abs_pos + 1 < size) {
                        /* Change RST sequence number (FFDn -> FFDm, m != n) */
                        uint8_t old_rst = data[abs_pos + 1];
                        uint8_t new_rst = 0xD0 + rng_int(ctx, 0, 7);
                        while (new_rst == old_rst)
                            new_rst = 0xD0 + rng_int(ctx, 0, 7);
                        data[abs_pos + 1] = new_rst;
                    }
                }
                rst_corrupted++;
            }
        }

        /* Partial cluster zeroing */
        if (cfg.partial_zero_prob > 0 && rng_bernoulli(ctx, cfg.partial_zero_prob)) {
            if (file.clusters.size() > 2) {
                /* Pick a random non-first cluster */
                int cl_idx = rng_int(ctx, 1, (int)file.clusters.size() - 1);
                uint64_t cl_offset = fat32_cluster_offset(geo, file.clusters[cl_idx]);
                if (cl_offset + bpc <= size) {
                    /* Zero first or last 512 bytes */
                    if (rng_bernoulli(ctx, 0.5))
                        memset(data + cl_offset, 0, 512);
                    else
                        memset(data + cl_offset + bpc - 512, 0, 512);
                    partial_zeroed++;
                }
            }
        }

        /* Misalignment: shift file data within first cluster so SOI starts
         * at a non-zero offset. Simulates partition misalignment or
         * non-standard sector boundaries. The original data at offset 0
         * becomes random noise, and the JPEG SOI moves to a later offset. */
        if (cfg.misalign_prob > 0 && rng_bernoulli(ctx, cfg.misalign_prob)) {
            uint64_t cl_offset = fat32_cluster_offset(geo, file.clusters[0]);
            if (cl_offset + bpc <= size) {
                int shift = rng_int(ctx, 128, cfg.misalign_max_offset);
                /* Align to 64-byte boundary for realistic sector alignment */
                shift = (shift / 64) * 64;
                if (shift < 64) shift = 64;
                if (shift > (int)bpc - 256) shift = bpc - 256;

                uint8_t *cl_data = data + cl_offset;
                /* Shift data right by 'shift' bytes */
                memmove(cl_data + shift, cl_data, bpc - shift);
                /* Fill gap with random noise (not zeros - zeros are too easy to skip) */
                for (int j = 0; j < shift; j++)
                    cl_data[j] = (uint8_t)rng_int(ctx, 0, 255);
                misaligned++;
            }
        }
    };

    log_progress(ctx, "  JPEG-targeted: processing %zu files + %zu deleted",
                 ctx.truth.files.size(), ctx.truth.deleted_files.size());
    for (auto &file : ctx.truth.files)
        process_file(file);
    for (auto &file : ctx.truth.deleted_files)
        process_file(file);

    log_progress(ctx, "  JPEG-targeted: DHT=%d DQT=%d SOF=%d stuffing=%d RST=%d partial_zero=%d misalign=%d SOS=%d (files: %zu+%zu)",
             dht_corrupted, dqt_corrupted, sof_corrupted,
             stuffing_corrupted, rst_corrupted, partial_zeroed, misaligned,
             sos_corrupted, ctx.truth.files.size(), ctx.truth.deleted_files.size());
    return true;
}
