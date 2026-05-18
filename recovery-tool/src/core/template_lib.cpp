/*
 * template_lib.cpp - Auto-build template library from intact JPEG headers
 *
 * Scans the first N JPEG_HEADER clusters, parses their headers, and collects
 * unique templates (by DQT fingerprint). These templates are used by
 * header_graft.cpp to recover files with corrupted/missing headers.
 *
 * Also extracts EXIF thumbnails for seeds that have them (used by
 * thumbnail MAE validation in scoring).
 */
#include "sdrecov.h"
#include <cstring>
#include <algorithm>

/* Max clusters to scan for template building */
static constexpr int MAX_TEMPLATE_SCAN = 500;

/* DQT distance threshold for "same camera/quality" */
static constexpr int DQT_SAME_THRESHOLD = 50;

/*
 * Build template library from intact JPEG headers found on disk.
 * Scans JPEG_HEADER clusters, parses headers, deduplicates by DQT fingerprint.
 */
void template_library_build(RecoveryContext &ctx)
{
    ctx.templates.clear();
    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;
    int scanned = 0;

    for (uint32_t i = 0; i < ctx.cluster_map.size() && scanned < MAX_TEMPLATE_SCAN; i++) {
        if (ctx.cluster_map[i].content_type != CTYPE_JPEG_HEADER)
            continue;

        uint32_t cluster = i + 2;
        const uint8_t *data = ctx.disk.cluster_ptr(cluster);
        if (!data) continue;
        if (data[0] != 0xFF || data[1] != 0xD8) continue;

        scanned++;

        /* Try to parse header (may need multiple clusters) */
        JpegTemplate tmpl;
        bool parsed = jpeg_parse_header(data, bpc, tmpl);

        if (!parsed) {
            /* Try sequential clusters for multi-cluster headers */
            std::vector<uint8_t> hbuf(data, data + bpc);
            for (int j = 1; j < 4 && !parsed; j++) {
                const uint8_t *d = ctx.disk.cluster_ptr(cluster + j);
                if (!d) break;
                hbuf.insert(hbuf.end(), d, d + bpc);
                parsed = jpeg_parse_header(hbuf.data(), hbuf.size(), tmpl);
            }
        }

        if (!parsed) continue;

        /* Check if this DQT fingerprint is already in the library */
        bool is_new = true;
        for (auto &existing : ctx.templates) {
            int dist_luma = dqt_distance(existing.dqt_luma, tmpl.dqt_luma);
            int dist_chroma = dqt_distance(existing.dqt_chroma, tmpl.dqt_chroma);
            if (dist_luma < DQT_SAME_THRESHOLD && dist_chroma < DQT_SAME_THRESHOLD) {
                is_new = false;
                break;
            }
        }

        if (is_new) {
            /* Extract camera name from EXIF if available */
            /* (Simple: scan APP1 for Make/Model strings - not a full TIFF parser) */
            tmpl.camera = "unknown";

            ctx.templates.push_back(std::move(tmpl));

            log_detail(ctx, "template %d: %dx%d, %d blk/MCU, DQT luma[0]=%d, from cluster %u",
                       (int)ctx.templates.size(),
                       ctx.templates.back().mcu_config.image_width,
                       ctx.templates.back().mcu_config.image_height,
                       ctx.templates.back().mcu_config.blocks_per_mcu,
                       ctx.templates.back().dqt_luma[0],
                       cluster);
        }
    }

    log_info(ctx, "Template library: %d unique templates from %d headers scanned",
             (int)ctx.templates.size(), scanned);
}

/*
 * Find the best matching template for a cluster of JPEG data.
 * Used by header grafting: given entropy data without a header, try each
 * template's Huffman tables and see which one decodes the most MCUs.
 *
 * Returns template index (0-based), or -1 if none work.
 */
int template_find_best(const RecoveryContext &ctx, const uint8_t *cluster_data, size_t len)
{
    int best_idx = -1;
    uint32_t best_mcus = 0;

    for (int t = 0; t < (int)ctx.templates.size(); t++) {
        auto &tmpl = ctx.templates[t];
        auto &cfg = tmpl.mcu_config;

        HuffCheckpoint state = {};
        if (cfg.restart_interval > 0)
            state.mcus_to_restart = cfg.restart_interval;

        HuffResult result = huff_validate_cluster(
            cluster_data, len, cfg,
            tmpl.dc_tables, tmpl.ac_tables, state);

        if (result.mcu_count > best_mcus) {
            best_mcus = result.mcu_count;
            best_idx = t;
        }
    }

    /* Require at least a few MCUs to be confident */
    if (best_mcus < 5) return -1;

    log_debug(ctx, "template match: best=%d (%u MCUs)", best_idx, best_mcus);
    return best_idx;
}
