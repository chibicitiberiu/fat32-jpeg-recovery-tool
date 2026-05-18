/*
 * chain_validate.cpp - Whole-chain validation and evaluation
 */
#include "sdrecov.h"

float evaluate_chain(const RecoveryContext &ctx, const Seed &seed,
                             const ChainResult &chain)
{
    return evaluate_chain_quality(ctx, seed, chain.clusters, chain.score,
                                  chain.mcus_recovered, chain.complete,
                                  chain.thumb_confidence);
}

/*
 * Whole-chain Huffman validation: concatenate all clusters and validate
 * the entire buffer from scratch, like the fast-path. This is much more
 * discriminative than per-cluster validation because errors accumulate
 * and become visible over longer sequences.
 *
 * Updates chain.mcus_recovered and chain.complete based on validation.
 * Returns the MCU ratio (0.0 = total failure, 1.0 = all MCUs validated).
 */
float validate_whole_chain(const RecoveryContext &ctx,
                                   const JpegTemplate &tmpl,
                                   ChainResult &chain)
{
    if (chain.clusters.empty()) return 0.0f;

    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;
    auto &cfg = tmpl.mcu_config;
    size_t header_len = tmpl.header_bytes.size();

    /* Concatenate all cluster data */
    std::vector<uint8_t> file_data;
    file_data.reserve(chain.clusters.size() * bpc);
    for (uint32_t cl : chain.clusters) {
        const uint8_t *d = ctx.disk.cluster_ptr(cl);
        if (!d) break;
        file_data.insert(file_data.end(), d, d + bpc);
    }

    if (file_data.size() <= header_len) return 0.0f;

    /* Fresh checkpoint (same as fast-path) */
    HuffCheckpoint vstate = {};
    if (cfg.restart_interval > 0)
        vstate.mcus_to_restart = cfg.restart_interval;
    if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
        vstate.scan_ss = cfg.scans[0].ss;
        vstate.scan_se = cfg.scans[0].se;
        vstate.scan_ah = cfg.scans[0].ah;
        vstate.scan_al = cfg.scans[0].al;
    }

    HuffResult vr = huff_validate_cluster(
        file_data.data() + header_len,
        file_data.size() - header_len,
        cfg, tmpl.dc_tables, tmpl.ac_tables, vstate);

    chain.mcus_recovered = vr.mcu_count;
    if (vr.passed && cfg.total_mcus > 0 &&
        vr.mcu_count >= cfg.total_mcus)
        chain.complete = true;

    /* If standard validation is poor, try tolerant (skip-and-resync).
     * This gives a better MCU count for files with bit errors in entropy
     * data, which helps with acceptance decisions. */
    if (ctx.features.tolerant_validate &&
        !vr.passed && cfg.total_mcus > 0 &&
        vr.mcu_count < cfg.total_mcus / 2) {
        uint32_t tolerant_mcus = huff_validate_tolerant(
            file_data.data() + header_len,
            file_data.size() - header_len,
            cfg, tmpl.dc_tables, tmpl.ac_tables, 10);
        if (tolerant_mcus > chain.mcus_recovered)
            chain.mcus_recovered = tolerant_mcus;
    }

    return cfg.total_mcus > 0
        ? (float)chain.mcus_recovered / cfg.total_mcus
        : (vr.passed ? 1.0f : 0.0f);
}
