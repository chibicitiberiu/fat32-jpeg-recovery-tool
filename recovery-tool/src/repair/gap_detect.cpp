/*
 * gap_detect.cpp - Detect gaps (zeroed/bad clusters) in recovered chains
 */
#include "sdrecov.h"

bool is_gap_cluster(const RecoveryContext &ctx, uint32_t cluster)
{
    uint32_t idx = cluster >= 2 ? cluster - 2 : 0;
    if (idx >= ctx.cluster_map.size()) return false;
    auto ct = ctx.cluster_map[idx].content_type;
    if (ct == CTYPE_BAD_SECTOR) return true;
    if (ct == CTYPE_EMPTY && (ctx.cluster_map[idx].flags & CF_IS_ZERO))
        return true;
    return false;
}

/*
 * Detect gaps using Huffman validation: if the chain validates well up to
 * cluster N but fails at cluster N, and cluster N has very different entropy
 * characteristics, it's likely corrupted. This catches partial corruption
 * (e.g., first 512 bytes zeroed) that cluster_map doesn't flag.
 */
std::vector<bool> detect_gaps_by_validation(const RecoveryContext &ctx,
    const std::vector<uint32_t> &clusters, const JpegTemplate &tmpl,
    size_t header_len)
{
    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;
    auto &cfg = tmpl.mcu_config;
    std::vector<bool> gaps(clusters.size(), false);

    /* Mark known gaps from cluster map */
    for (size_t i = 0; i < clusters.size(); i++) {
        if (is_gap_cluster(ctx, clusters[i]))
            gaps[i] = true;
    }

    /* Walk through the chain validating incrementally.
     * Where validation suddenly fails (error at cluster boundary),
     * the cluster likely has corruption. */
    size_t first_data = header_len / bpc;
    if (header_len % bpc != 0) first_data++;

    std::vector<uint8_t> buf;
    for (size_t ci = 0; ci < clusters.size(); ci++) {
        const uint8_t *d = ctx.disk.cluster_ptr(clusters[ci]);
        if (d) buf.insert(buf.end(), d, d + bpc);
        else { buf.insert(buf.end(), bpc, 0); gaps[ci] = true; }
    }

    if (buf.size() <= header_len) return gaps;

    /* Validate entire buffer, then validate without each suspect cluster
     * to see if removing it improves MCU count */
    HuffCheckpoint full_state = {};
    if (cfg.restart_interval > 0) full_state.mcus_to_restart = cfg.restart_interval;
    if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
        full_state.scan_ss = cfg.scans[0].ss;
        full_state.scan_se = cfg.scans[0].se;
        full_state.scan_ah = cfg.scans[0].ah;
        full_state.scan_al = cfg.scans[0].al;
    }
    HuffResult full_r = huff_validate_cluster(
        buf.data() + header_len, buf.size() - header_len,
        cfg, tmpl.dc_tables, tmpl.ac_tables, full_state);

    /* If validation failed, find which cluster caused it */
    if (!full_r.passed && full_r.offset > 0) {
        size_t fail_byte = header_len + full_r.offset;
        size_t fail_cluster = fail_byte / bpc;
        if (fail_cluster > first_data && fail_cluster < clusters.size())
            gaps[fail_cluster] = true;
    }

    return gaps;
}
