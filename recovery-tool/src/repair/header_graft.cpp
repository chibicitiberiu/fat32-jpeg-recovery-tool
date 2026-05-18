/*
 * header_graft.cpp - Template matching and header reconstruction
 *
 * When a seed's JPEG header is corrupted (jpeg_parse_header fails),
 * try each template from the library: prepend the template's header
 * to the seed's entropy data and attempt Huffman validation.
 * Pick the template that decodes the most MCUs.
 *
 * This recovers files that signature carvers find (FFD8FF present)
 * but whose DHT/DQT/SOF markers are corrupted beyond parsing.
 */
#include "sdrecov.h"
#include <cstring>
#include <algorithm>

/* Standard Annex K tables (defined in engine.cpp) */
extern HuffTable g_std_dc[2], g_std_ac[2];
extern bool g_std_tables_built;
extern void build_standard_tables();

/*
 * Try to graft a template header onto a seed's data.
 *
 * Strategy:
 *   1. Find where the entropy data likely starts in the seed's cluster
 *      (scan for SOS marker FFDa, or just try from various offsets)
 *   2. Prepend the template's header bytes
 *   3. Validate with Huffman decoder
 *   4. Return the template index and entropy offset that worked best
 *
 * Returns true if a template matched, filling out tmpl_out with the
 * working template and entropy_offset with where data starts.
 */
bool header_graft(RecoveryContext &ctx, uint32_t start_cluster,
                  JpegTemplate &tmpl_out, size_t &entropy_offset)
{
    if (ctx.templates.empty()) return false;

    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;
    const uint8_t *data = ctx.disk.cluster_ptr(start_cluster);
    if (!data) return false;

    /* Require JPEG SOI signature (FF D8) at the start.
     * Without SOI, this cluster is not JPEG data and grafting would
     * just produce false positives by matching random data. */
    if (data[0] != 0xFF || data[1] != 0xD8) {
        log_debug(ctx, "header graft: no SOI at cluster %u, skipping", start_cluster);
        return false;
    }

    /* Find the seed's expected size for template filtering */
    uint32_t expected_size = 0;
    for (auto &seed : ctx.seeds) {
        if (seed.start_cluster == start_cluster) {
            expected_size = seed.expected_size;
            break;
        }
    }

    /*
     * Find potential entropy data start points.
     * In a JPEG with corrupted header, the entropy data is still there
     * but the markers before it are garbled. Common patterns:
     *   - SOS marker (FFDA) is intact but other markers are broken
     *   - SOI (FFD8) is present but everything between SOI and entropy is corrupt
     *   - Data starts right after a partial header
     *
     * We try a few strategies to find the entropy start:
     */
    std::vector<size_t> try_offsets;

    /* Strategy 1: Scan for SOS marker (FFDA) */
    for (size_t i = 0; i + 3 < bpc; i++) {
        if (data[i] == 0xFF && data[i + 1] == 0xDA) {
            /* SOS found - entropy starts after the SOS header */
            uint16_t sos_len = (uint16_t(data[i + 2]) << 8) | data[i + 3];
            if (i + 2 + sos_len < bpc) {
                try_offsets.push_back(i + 2 + sos_len);
            }
        }
    }

    /* Strategy 2: Common header sizes for phone cameras */
    /* EXIF headers are typically 4-15KB, so entropy starts around there */
    for (size_t off = 300; off < std::min((size_t)bpc, (size_t)20000); off += 100) {
        try_offsets.push_back(off);
    }

    /* Deduplicate and sort */
    std::sort(try_offsets.begin(), try_offsets.end());
    try_offsets.erase(std::unique(try_offsets.begin(), try_offsets.end()), try_offsets.end());

    int best_template = -1;
    size_t best_offset = 0;
    uint32_t best_mcus = 0;

    /* Also try standard Annex K tables (~90% of cameras use these) */
    build_standard_tables();

    for (int t = 0; t < (int)ctx.templates.size(); t++) {
        auto &tmpl = ctx.templates[t];
        auto &cfg = tmpl.mcu_config;

        for (size_t off : try_offsets) {
            if (off >= bpc) continue;
            size_t entropy_len = bpc - off;
            if (entropy_len < 100) continue;

            /* Try template's own tables */
            HuffCheckpoint state = {};
            if (cfg.restart_interval > 0)
                state.mcus_to_restart = cfg.restart_interval;

            HuffResult result = huff_validate_cluster(
                data + off, entropy_len, cfg,
                tmpl.dc_tables, tmpl.ac_tables, state);

            /* Score = MCU count, penalized if template size doesn't match expected */
            uint32_t score = result.mcu_count;
            if (expected_size > 0 && cfg.total_mcus > 0) {
                float est_size = (float)cfg.total_mcus * 50.0f; /* rough avg bytes/MCU */
                float ratio = (float)expected_size / est_size;
                if (ratio < 0.1f || ratio > 10.0f)
                    score = score / 4; /* heavy penalty for size mismatch */
            }

            if (score > best_mcus) {
                best_mcus = score;
                best_template = t;
                best_offset = off;
            }

            /* Also try standard Annex K tables with this template's config */
            HuffCheckpoint std_state = {};
            if (cfg.restart_interval > 0)
                std_state.mcus_to_restart = cfg.restart_interval;

            HuffResult std_result = huff_validate_cluster(
                data + off, entropy_len, cfg,
                g_std_dc, g_std_ac, std_state);

            uint32_t std_score = std_result.mcu_count;
            if (expected_size > 0 && cfg.total_mcus > 0) {
                float est_size = (float)cfg.total_mcus * 50.0f;
                float ratio = (float)expected_size / est_size;
                if (ratio < 0.1f || ratio > 10.0f)
                    std_score = std_score / 4;
            }

            if (std_score > best_mcus) {
                best_mcus = std_score;
                best_template = t;
                best_offset = off;
            }

            if (best_mcus > 500) break;
        }

        if (best_mcus > 200) break;
    }

    if (best_template < 0 || best_mcus < 30) {
        log_debug(ctx, "header graft: no template matched (best %u MCUs)", best_mcus);
        return false;
    }

    tmpl_out = ctx.templates[best_template];
    entropy_offset = best_offset;

    log_detail(ctx, "header graft: template %d matched at offset %zu (%u MCUs), "
               "%dx%d %d blk/MCU",
               best_template, best_offset, best_mcus,
               tmpl_out.mcu_config.image_width, tmpl_out.mcu_config.image_height,
               tmpl_out.mcu_config.blocks_per_mcu);

    return true;
}
