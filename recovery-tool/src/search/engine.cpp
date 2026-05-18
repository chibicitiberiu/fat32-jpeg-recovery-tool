/*
 * engine.cpp - DFS search with backtracking
 *
 * For each seed, explores multiple chain paths through the cluster space.
 * At each step where multiple candidates pass Huffman validation, saves
 * a branch point. If the chain ends poorly (low score, thumbnail mismatch),
 * backtracks to the last branch point and tries the next-best candidate.
 *
 * Thumbnail validation is used as an active signal: after building a complete
 * chain, validate against the EXIF thumbnail. If it fails, reject the chain
 * and try alternative paths.
 */
#include "sdrecov.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

/* Shorthand for feature flag checks */
#define FEAT(ctx, flag) ((ctx).features.flag)

/* Standard tables moved to src/core/standard_tables.cpp */
#include <unordered_set>
#include <thread>
#include <mutex>
#include <atomic>

/* ChainResult and BranchPoint are in sdrecov.h */

/* evaluate_chain, validate_whole_chain -> chain_validate.cpp */

static std::vector<ChainVariant> build_chain_multi(RecoveryContext &ctx, const Seed &seed)
{
    std::vector<ChainVariant> all_variants;
    ChainResult best_chain;
    float best_eval = -1.0f;

    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;

    const uint8_t *first_data = ctx.disk.cluster_ptr(seed.start_cluster);
    if (!first_data) return all_variants;

    /* Handle mid-cluster SOI: adjust data pointer */
    const uint8_t *jpeg_start = first_data + seed.soi_offset;
    size_t jpeg_avail = bpc - seed.soi_offset;

    /* ---- Parse JPEG header ---- */
    JpegTemplate tmpl;
    bool parsed = jpeg_parse_header(jpeg_start, jpeg_avail, tmpl);

    if (!parsed) {
        std::vector<uint8_t> hbuf(jpeg_start, jpeg_start + jpeg_avail);

        /* Strategy 1: follow FAT chain */
        uint32_t cl = seed.start_cluster;
        if (cl < ctx.fat.count()) cl = ctx.fat.merged[cl];
        for (int i = 1; i < 16 && !parsed && cl >= 2 && cl <= ctx.disk.geo.total_clusters + 1; i++) {
            const uint8_t *d = ctx.disk.cluster_ptr(cl);
            if (!d) break;
            hbuf.insert(hbuf.end(), d, d + bpc);
            parsed = jpeg_parse_header(hbuf.data(), hbuf.size(), tmpl);
            if (cl < ctx.fat.count()) cl = ctx.fat.merged[cl]; else break;
        }

        /* Strategy 2: sequential */
        if (!parsed) {
            hbuf.assign(jpeg_start, jpeg_start + jpeg_avail);
            for (int i = 1; i < 16 && !parsed; i++) {
                const uint8_t *d = ctx.disk.cluster_ptr(seed.start_cluster + i);
                if (!d) break;
                hbuf.insert(hbuf.end(), d, d + bpc);
                parsed = jpeg_parse_header(hbuf.data(), hbuf.size(), tmpl);
            }
        }
    }

    bool was_grafted = false;
    size_t graft_entropy_off = 0;
    std::vector<uint8_t> graft_header_bytes;

    if (!parsed && FEAT(ctx, header_graft)) {
        /* Try header grafting */
        size_t graft_offset = 0;
        if (header_graft(ctx, seed.start_cluster, tmpl, graft_offset)) {
            /* Sanity check: reject graft if template's MCU count implies a
             * file size wildly different from the expected size. A 550x231
             * template (2001 MCUs ≈ 35KB entropy) shouldn't be grafted onto
             * a 5MB file. */
            bool graft_ok = true;
            if (seed.expected_size > 0 && tmpl.mcu_config.total_mcus > 0) {
                /* Rough estimate: each MCU produces ~20-200 bytes of entropy.
                 * Use a generous range to avoid false rejections. */
                size_t min_est = (size_t)tmpl.mcu_config.total_mcus * 10;
                size_t max_est = (size_t)tmpl.mcu_config.total_mcus * 300;
                if (seed.expected_size < min_est || seed.expected_size > max_est) {
                    graft_ok = false;
                    log_debug(ctx, "seed %u: rejecting graft - size mismatch "
                              "(expected %u bytes, template %u MCUs)",
                              seed.start_cluster, seed.expected_size,
                              tmpl.mcu_config.total_mcus);
                }
            }
            if (graft_ok) {
                parsed = true;
                was_grafted = true;
                graft_entropy_off = graft_offset;
                graft_header_bytes = tmpl.header_bytes;
                tmpl.header_bytes.resize(graft_offset);
                log_detail(ctx, "seed %u (%s): recovered via header grafting",
                           seed.start_cluster, seed.filename.c_str());
            }
        }
    }

    if (!parsed) {
        log_detail(ctx, "seed %u (%s): header parse + graft failed",
                   seed.start_cluster, seed.filename.c_str());
        return all_variants;
    }

    auto &cfg = tmpl.mcu_config;
    log_detail(ctx, "seed %u: %dx%d, %d comp, %d blk/MCU, %u MCUs, hdr=%zu bytes, rst=%d",
               seed.start_cluster, cfg.image_width, cfg.image_height,
               cfg.num_components, cfg.blocks_per_mcu, cfg.total_mcus,
               tmpl.header_bytes.size(), cfg.restart_interval);

    /* ---- Build initial state ---- */
    HuffCheckpoint init_state = {};
    if (cfg.restart_interval > 0)
        init_state.mcus_to_restart = cfg.restart_interval;

    /* Initialize progressive scan state from first scan's parameters */
    if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
        init_state.scan_ss = cfg.scans[0].ss;
        init_state.scan_se = cfg.scans[0].se;
        init_state.scan_ah = cfg.scans[0].ah;
        init_state.scan_al = cfg.scans[0].al;
        init_state.current_scan = 0;
        init_state.eob_run = 0;
    }

    /* If seed has no expected_size (signature-scan, no directory entry),
     * estimate from image dimensions. Typical JPEG = raw_size / 10..20.
     * Use raw_size / 5 as a generous upper estimate. */
    /* For size-based decisions, use directory entry size when available.
     * For signature-scan seeds (no directory entry), estimate a plausible
     * range from image dimensions. JPEG compression ratios vary widely
     * (1:50 for simple images to 1:3 for noisy photos), so we use:
     *   min_size = raw / 20  (very compressed)
     *   max_size = raw       (theoretical max, uncompressed)
     * The effective_expected_size is set to max for "is chain too small?"
     * checks, since we'd rather over-scan than miss data. */
    uint32_t effective_expected_size = seed.expected_size;
    uint32_t estimated_min_size = 0;
    uint32_t estimated_max_size = 0;
    if (cfg.image_width > 0 && cfg.image_height > 0) {
        uint64_t raw = (uint64_t)cfg.image_width * cfg.image_height * cfg.num_components;
        estimated_min_size = (uint32_t)std::min(raw / 20, (uint64_t)UINT32_MAX);
        estimated_max_size = (uint32_t)std::min(raw, (uint64_t)UINT32_MAX);
    }
    if (FEAT(ctx, size_estimation) && effective_expected_size == 0 && estimated_max_size > 0) {
        effective_expected_size = estimated_max_size;
        log_debug(ctx, "seed %u: estimated size range %u-%u bytes from %dx%d",
                  seed.start_cluster, estimated_min_size, estimated_max_size,
                  cfg.image_width, cfg.image_height);
    }

    /* header_len is relative to SOI. Actual position in cluster data
     * is soi_offset + header_len. */
    size_t header_len = seed.soi_offset + tmpl.header_bytes.size();
    uint32_t header_clusters = (header_len + bpc - 1) / bpc;

    /* Collect header clusters */
    std::vector<uint32_t> header_chain;
    uint32_t cl = seed.start_cluster;
    for (uint32_t i = 0; i < header_clusters && cl >= 2; i++) {
        header_chain.push_back(cl);
        if (cl < ctx.fat.count()) cl = ctx.fat.merged[cl]; else break;
        if (cl < 2 || cl > ctx.disk.geo.total_clusters + 1) break;
    }

    /* Validate initial entropy */
    size_t entropy_off = header_len % bpc;
    if (entropy_off > 0 && !header_chain.empty()) {
        uint32_t last_hdr = header_chain.back();
        const uint8_t *cdata = ctx.disk.cluster_ptr(last_hdr);
        if (!cdata) return all_variants;

        size_t tail = bpc - entropy_off;
        HuffResult r = huff_validate_cluster(
            cdata + entropy_off, tail, cfg, tmpl.dc_tables, tmpl.ac_tables, init_state);

        if (!r.passed) {
            /* Try RST recovery */
            if (FEAT(ctx, rst_recovery) && cfg.restart_interval > 0) {
                uint32_t extra = rst_skip_and_resume(
                    cdata + entropy_off, tail, cfg,
                    tmpl.dc_tables, tmpl.ac_tables, init_state, r.offset);
                if (extra > 0) {
                    log_detail(ctx, "seed %u: RST recovery rescued %u MCUs",
                               seed.start_cluster, extra);
                }
            }
            if (init_state.mcu_count == 0 && FEAT(ctx, annex_k_retry)) {
                /* Try standard Annex K tables before giving up.
                 * The file's DHT may have been substituted during parse
                 * but still doesn't match the actual encoding. */
                build_standard_tables();
                HuffCheckpoint retry_state = {};
                if (cfg.restart_interval > 0)
                    retry_state.mcus_to_restart = cfg.restart_interval;
                if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
                    retry_state.scan_ss = cfg.scans[0].ss;
                    retry_state.scan_se = cfg.scans[0].se;
                    retry_state.scan_ah = cfg.scans[0].ah;
                    retry_state.scan_al = cfg.scans[0].al;
                }
                HuffResult retry_r = huff_validate_cluster(
                    cdata + entropy_off, tail, cfg,
                    g_std_dc, g_std_ac, retry_state);

                if (retry_r.mcu_count > 0 || retry_r.passed) {
                    log_detail(ctx, "seed %u: Annex K retry rescued initial entropy (%u MCUs)",
                               seed.start_cluster, retry_r.mcu_count);
                    init_state = retry_state;
                    for (int i = 0; i < MAX_HUFF_TABLES; i++) {
                        tmpl.dc_tables[i] = g_std_dc[std::min(i, 1)];
                        tmpl.ac_tables[i] = g_std_ac[std::min(i, 1)];
                    }
                } else {
                    log_detail(ctx, "seed %u: initial entropy failed (err=%d at %zu)",
                               seed.start_cluster, r.error_type, r.offset);
                    return all_variants;
                }
            }
            /* Hard reject if still 0 MCUs and retry was disabled or failed */
            if (init_state.mcu_count == 0 && !FEAT(ctx, annex_k_retry)) {
                log_detail(ctx, "seed %u: initial entropy failed (err=%d at %zu)",
                           seed.start_cluster, r.error_type, r.offset);
                return all_variants;
            }
        }
        /* Adjust byte_pos to be relative to the full cluster, not the entropy tail.
         * This is critical for the overlap buffer: saved.byte_pos tells us where
         * in the cluster the reader stopped, so we can compute the unread tail. */
        init_state.byte_pos = entropy_off + init_state.byte_pos;
    }

    if (header_chain.empty()) return all_variants;
    /* Fast-path: try FAT chain directly (extracted to fast_path.cpp) */
    if (FEAT(ctx, fast_path)) {
        auto fp = try_fast_path(ctx, seed, tmpl, header_chain, header_len,
                                was_grafted, graft_header_bytes, graft_entropy_off,
                                effective_expected_size, estimated_min_size,
                                estimated_max_size);

        /* Track the best chain for DFS baseline (before moving variants) */
        if (fp.best_eval > best_eval && !fp.variants.empty()) {
            best_chain = fp.variants[0].chain; /* best variant is first */
            best_eval = fp.best_eval;
        }

        /* Collect fast-path variants */
        for (auto &v : fp.variants)
            all_variants.push_back(std::move(v));

        if (fp.should_return && ctx.search_mode == RecoveryContext::SEARCH_GREEDY) {
            /* Run thumbnail validation on all variants */
            for (auto &v : all_variants) {
                if (seed.has_thumbnail && !v.chain.clusters.empty()) {
                    std::vector<uint8_t> recovered;
                    for (auto c : v.chain.clusters) {
                        const uint8_t *d = ctx.disk.cluster_ptr(c);
                        if (!d) break;
                        recovered.insert(recovered.end(), d, d + bpc);
                    }
                    v.chain.thumb_confidence = FEAT(ctx, thumbnail_validate)
                        ? thumbnail_validate(ctx, seed, recovered.data(), recovered.size())
                        : -1.0f;
                }
            }
            return all_variants;
        }
    }

    /* ---- DFS with backtracking (fallback for broken FAT chains) ---- */
    /*
     * We explore the search tree depth-first. At each step where multiple
     * candidates pass Huffman validation, we save a branch point.
     * We first follow the best path greedily. When the chain ends,
     * we evaluate it (including thumbnail). If it's not good enough
     * and we have unexplored branches, we backtrack and try alternatives.
     */

    int max_backtracks;
    float backtrack_threshold;

    switch (ctx.search_mode) {
    case RecoveryContext::SEARCH_FULL:
        max_backtracks = 999999; /* effectively unlimited */
        backtrack_threshold = 999.0f; /* always backtrack = explore everything */
        break;
    case RecoveryContext::SEARCH_BEAM:
        max_backtracks = ctx.beam_width * 50; /* generous but bounded */
        backtrack_threshold = 999.0f; /* always try alternatives */
        break;
    default: /* SEARCH_GREEDY */
        max_backtracks = ctx.max_backtracks;
        backtrack_threshold = 0.7f; /* only backtrack if chain is poor */
        break;
    }
    /* P2: Expected file size termination - allow 20% slack */
    int max_chain_len = seed.expected_clusters > 0
        ? (int)(seed.expected_clusters * 1.2) : 1000;

    std::vector<uint32_t> cand_cl(ctx.max_candidates);
    std::vector<float> cand_pri(ctx.max_candidates);

    /* explore_path is now in dfs_explore.cpp.
     * Define a local wrapper that captures the shared variables. */
    auto explore_path = [&](std::vector<uint32_t> chain_clusters,
                            HuffCheckpoint state, float score_accum,
                            std::vector<BranchPoint> &branches,
                            std::vector<uint8_t> &chain_buf) -> ChainResult {
        return ::explore_path(ctx, tmpl, seed, std::move(chain_clusters),
                              state, score_accum, branches, chain_buf, max_chain_len);
    }; /* end wrapper */

    /* explore_path body -> dfs_explore.cpp */

    /* First path: greedy best-first.
     * Build initial chain_buf from header clusters for incremental validation. */
    std::vector<BranchPoint> branches;
    std::vector<uint8_t> chain_buf;
    chain_buf.reserve(max_chain_len * bpc);
    for (uint32_t hc : header_chain) {
        const uint8_t *d = ctx.disk.cluster_ptr(hc);
        if (d) chain_buf.insert(chain_buf.end(), d, d + bpc);
    }

    ChainResult dfs_chain = explore_path(header_chain, init_state, 0.0f, branches, chain_buf);

    /* Whole-chain validation for final accuracy check */
    float dfs_mcu_ratio = validate_whole_chain(ctx, tmpl, dfs_chain);
    log_debug(ctx, "seed %u: DFS chain whole-validate: %.1f%% MCUs (%u/%u)",
              seed.start_cluster, dfs_mcu_ratio * 100,
              dfs_chain.mcus_recovered, cfg.total_mcus);

    /* Use DFS result only if it's better than any fast-path baseline */
    float dfs_eval = evaluate_chain(ctx, seed, dfs_chain);
    if (dfs_eval > best_eval) {
        best_chain = std::move(dfs_chain);
        best_eval = dfs_eval;
    }

    /* Thumbnail validation on the best chain.
     * Cluster data already includes the header - don't prepend header_bytes. */
    if (seed.has_thumbnail && !best_chain.clusters.empty()) {
        std::vector<uint8_t> recovered;
        for (auto c : best_chain.clusters) {
            const uint8_t *d = ctx.disk.cluster_ptr(c);
            if (!d) break;
            recovered.insert(recovered.end(), d, d + bpc);
        }
        best_chain.thumb_confidence = FEAT(ctx, thumbnail_validate) ? thumbnail_validate(
            ctx, seed, recovered.data(), recovered.size()) : -1.0f;
    }

    best_eval = evaluate_chain(ctx, seed, best_chain);

    log_debug(ctx, "path 0: %zu clusters, %u MCUs, score=%.2f, thumb=%.2f, eval=%.2f%s",
              best_chain.clusters.size(), best_chain.mcus_recovered,
              best_chain.score, best_chain.thumb_confidence, best_eval,
              best_chain.complete ? " (complete)" : "");

    /* Backtrack and explore alternatives if the first path was poor */
    int backtracks = 0;
    auto bt_start = std::chrono::steady_clock::now();
    constexpr int BT_TIMEOUT_MS = 15000; /* 15 seconds per seed */

    while (backtracks < max_backtracks && !branches.empty() && best_eval < backtrack_threshold) {
        /* Pop the most recent branch point */
        BranchPoint bp = std::move(branches.back());
        branches.pop_back();

        if (bp.candidates.empty()) continue;

        /* Try the next alternative from this branch */
        uint32_t alt_cluster = bp.candidates[0];
        float alt_score = bp.cand_scores[0];
        bp.candidates.erase(bp.candidates.begin());
        bp.cand_scores.erase(bp.cand_scores.begin());

        /* If there are more alternatives, push the branch back */
        if (!bp.candidates.empty())
            branches.push_back(bp);

        /* Reconstruct chain_buf from branch point and validate alternative */
        const uint8_t *cdata = ctx.disk.cluster_ptr(alt_cluster);
        if (!cdata) continue;

        /* Rebuild chain_buf up to the branch point */
        std::vector<uint8_t> alt_chain_buf;
        alt_chain_buf.reserve(bp.chain_buf_size + bpc * 20);
        for (uint32_t bc : bp.chain_so_far) {
            const uint8_t *d = ctx.disk.cluster_ptr(bc);
            if (d) alt_chain_buf.insert(alt_chain_buf.end(), d, d + bpc);
        }
        /* Trim to saved size (in case clusters have different effective sizes) */
        if (alt_chain_buf.size() > bp.chain_buf_size)
            alt_chain_buf.resize(bp.chain_buf_size);

        /* Append alternative cluster and validate */
        alt_chain_buf.insert(alt_chain_buf.end(), cdata, cdata + bpc);

        HuffCheckpoint alt_state = bp.state;
        HuffResult r = huff_validate_cluster(
            alt_chain_buf.data(), alt_chain_buf.size(),
            cfg, tmpl.dc_tables, tmpl.ac_tables, alt_state);

        if (!r.passed) continue;

        /* Build chain from this alternative */
        auto alt_chain_start = bp.chain_so_far;
        alt_chain_start.push_back(alt_cluster);

        std::vector<BranchPoint> alt_branches;
        ChainResult alt_chain = explore_path(
            alt_chain_start, alt_state, bp.score_so_far + alt_score,
            alt_branches, alt_chain_buf);

        /* Whole-chain validation for accurate comparison */
        validate_whole_chain(ctx, tmpl, alt_chain);

        /* Thumbnail validation (clusters include header data) */
        if (seed.has_thumbnail && !alt_chain.clusters.empty()) {
            std::vector<uint8_t> recovered;
            for (auto c : alt_chain.clusters) {
                const uint8_t *d = ctx.disk.cluster_ptr(c);
                if (!d) break;
                recovered.insert(recovered.end(), d, d + bpc);
            }
            alt_chain.thumb_confidence = FEAT(ctx, thumbnail_validate) ? thumbnail_validate(
                ctx, seed, recovered.data(), recovered.size()) : -1.0f;
        }

        float alt_eval = evaluate_chain(ctx, seed, alt_chain);
        backtracks++;

        /* Timeout safety for full backtracking */
        auto bt_elapsed = std::chrono::steady_clock::now() - bt_start;
        if (std::chrono::duration_cast<std::chrono::milliseconds>(bt_elapsed).count() > BT_TIMEOUT_MS) {
            log_detail(ctx, "seed %u: backtracking timeout after %d paths",
                       seed.start_cluster, backtracks);
            break;
        }

        log_debug(ctx, "path %d (backtrack at step %d, cluster %u): "
                  "%zu clusters, %u MCUs, score=%.2f, thumb=%.2f, eval=%.2f%s",
                  backtracks, bp.step, alt_cluster,
                  alt_chain.clusters.size(), alt_chain.mcus_recovered,
                  alt_chain.score, alt_chain.thumb_confidence, alt_eval,
                  alt_chain.complete ? " (complete)" : "");

        if (alt_eval > best_eval) {
            best_chain = std::move(alt_chain);
            best_eval = alt_eval;
            /* Merge alternative branches into the pool */
            for (auto &ab : alt_branches)
                branches.push_back(std::move(ab));
        }
    }

    if (backtracks > 0) {
        log_detail(ctx, "seed %u: explored %d alternative paths, best eval=%.2f",
                   seed.start_cluster, backtracks + 1, best_eval);
    }

    /* Store graft info for write_recovered */
    if (was_grafted) {
        best_chain.grafted = true;
        best_chain.graft_header = std::move(graft_header_bytes);
        best_chain.entropy_offset = graft_entropy_off;
    }

    /* Add DFS best chain as a variant (may duplicate a fast-path variant) */
    if (!best_chain.clusters.empty()) {
        ChainVariant dfs_var;
        dfs_var.chain = std::move(best_chain);
        dfs_var.tag = "dfs";
        dfs_var.confidence = best_eval > 0 ? best_eval : 0.5f;
        /* Deduplicate: only add if cluster list differs from existing */
        bool dup = false;
        for (auto &v : all_variants) {
            if (v.chain.clusters == dfs_var.chain.clusters) { dup = true; break; }
        }
        if (!dup)
            all_variants.push_back(std::move(dfs_var));
    }

    /* Sort by confidence descending - best variant first */
    std::sort(all_variants.begin(), all_variants.end(),
              [](const ChainVariant &a, const ChainVariant &b) {
                  return a.confidence > b.confidence;
              });

    return all_variants;
}

/*
 * is_gap_cluster, detect_gaps_by_validation -> repair/gap_detect.cpp
 * write_recovered, write_recovered_with_rst -> search/file_writer.cpp
 */

int engine_recover(RecoveryContext &ctx)
{
    ctx.claimed_score.resize(ctx.disk.geo.total_clusters, 0.0f);

    int total = (int)ctx.seeds.size();
    int start = std::min(ctx.seeds_offset, total);
    int end = (ctx.seeds_limit > 0) ? std::min(start + ctx.seeds_limit, total) : total;
    int range = end - start;

    log_progress(ctx, "[stage 2/2] Processing seeds %d..%d of %d (%d threads)",
                 start, end - 1, total, ctx.threads);

    /* Shared counters */
    std::atomic<int> recovered{0}, skipped{0}, failed{0}, header_fail{0};
    std::atomic<int> progress{0};
    std::mutex write_mutex; /* protects file writing and file numbering */
    int file_num = 0;

    /* Worker function: process a range of seeds */
    auto worker = [&](int w_start, int w_end) {
        for (int i = w_start; i < w_end; i++) {
            auto &seed = ctx.seeds[i];

            /* Progress reporting */
            int done = progress.fetch_add(1);
            if (done % 100 == 0) {
                log_progress(ctx, "[stage 2/2] %d/%d seeds (%d recovered, %d failed, %d header-fail, %d skipped)",
                             done, range, recovered.load(), failed.load(),
                             header_fail.load(), skipped.load());
            }

            /* Skip claimed - but be lenient. A claimed cluster means another
             * chain already uses it, but it might be wrong (cross-link, wrong
             * dir entry). Only skip when the existing claim is very strong. */
            if (seed.start_cluster >= 2) {
                uint32_t idx = seed.start_cluster - 2;
                if (idx < ctx.claimed_score.size() && ctx.claimed_score[idx] > ctx.min_score) {
                    float existing = ctx.claimed_score[idx];
                    /* Allow processing if:
                     * - This seed has high confidence (dir entry + signature match)
                     * - The existing claim is weak (< 0.8)
                     * - This seed came from a different source than the claimant */
                    bool can_proceed = (seed.confidence >= CONF_HIGH) ||
                                       (seed.source == SEED_BOTH) ||
                                       (existing < 0.8f);
                    if (!can_proceed) {
                        skipped++;
                        continue;
                    }
                    log_debug(ctx, "seed %u proceeding despite claim (%.2f, conf=%d)",
                              seed.start_cluster, existing, seed.confidence);
                }
            }

            /* Skip non-JPEG signatures */
            if (seed.source == SEED_SIGNATURE_SCAN) {
                uint32_t cidx = seed.start_cluster - 2;
                if (cidx < ctx.cluster_map.size() && ctx.cluster_map[cidx].content_type != CTYPE_JPEG_HEADER) {
                    skipped++;
                    continue;
                }
            }

            auto variants = build_chain_multi(ctx, seed);

            if (variants.empty()) {
                header_fail++;
                continue;
            }

            if (variants[0].chain.clusters.size() < 2 &&
                variants[0].chain.score < ctx.min_score) {
                failed++;
                continue;
            }

            /* Serialize file writing */
            {
                std::lock_guard<std::mutex> lock(write_mutex);

                /* Decide how many variants to emit:
                 * - Single variant when best is high confidence
                 * - Multiple when confidence is ambiguous */
                bool emit_all = variants.size() > 1 &&
                                variants[0].confidence < 0.9f;
                int limit = emit_all ? (int)variants.size() : 1;

                bool any_written = false;
                auto &best = variants[0].chain;

                /* RST injection for severely incomplete recoveries only.
                 * When < 70% of MCUs recovered, RST bridges gaps so viewers
                 * show data on both sides of corruption. Above 70%, the normal
                 * version is usually better (RST DC resets create artifacts). */
                bool severely_partial = (best.total_mcus > 0 &&
                    best.mcus_recovered < best.total_mcus * 7 / 10);
                if (severely_partial && !best.clusters.empty()) {
                    file_num++;
                    bool rst_ok = write_recovered_with_rst(
                        ctx, seed, best, file_num);
                    if (rst_ok) any_written = true;
                }

                /* Write normal version(s) */
                for (int vi = 0; vi < limit; vi++) {
                    file_num++;
                    bool written = write_recovered_variant(
                        ctx, seed, variants[vi], file_num, vi);
                    if (written) any_written = true;
                }

                if (any_written)
                    recovered++;
                else
                    failed++;
            }
        }
    };

    /* Single-threaded for small runs or threads=1 */
    if (ctx.threads <= 1 || range < 50) {
        worker(start, end);
    } else {
        /* Partition seeds across threads */
        std::vector<std::thread> threads;
        int per_thread = range / ctx.threads;

        for (int t = 0; t < ctx.threads; t++) {
            int t_start = start + t * per_thread;
            int t_end = (t == ctx.threads - 1) ? end : t_start + per_thread;
            threads.emplace_back(worker, t_start, t_end);
        }

        for (auto &t : threads) t.join();
    }

    /* Final progress */
    log_progress(ctx, "[stage 2/2] Done: %d recovered, %d failed, %d header-fail, %d skipped",
                 recovered.load(), failed.load(), header_fail.load(), skipped.load());
    dfs_print_filter_stats(ctx);
    return recovered.load();
}
