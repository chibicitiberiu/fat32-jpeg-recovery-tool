/*
 * dfs_explore.cpp - DFS path exploration for chain building
 *
 * The core DFS step function: at each step, enumerates candidate clusters,
 * validates each against the growing chain buffer, applies pruning filters,
 * scores candidates, and picks the best. Saves branch points for backtracking.
 */
#include "sdrecov.h"
#include <cstring>
#include <algorithm>
#include <unordered_set>
#include <atomic>

#define FEAT(ctx, flag) ((ctx).features.flag)

/* DFS filter hit counters (for diagnostics via log) */
static std::atomic<int> g_dfs_calls{0};
static std::atomic<int> g_dfs_cands_evaluated{0};
static std::atomic<int> g_filter_rst{0};
static std::atomic<int> g_filter_ff00{0};
static std::atomic<int> g_filter_entropy{0};
static std::atomic<int> g_filter_boundary{0};
static std::atomic<int> g_filter_huff_fail{0};
static std::atomic<int> g_filter_mcu_noinc{0};
static std::atomic<int> g_filter_dc_bounds{0};
static std::atomic<int> g_filter_mcu_rate{0};
static std::atomic<int> g_filter_oversize{0};
static std::atomic<int> g_cands_accepted{0};

void dfs_print_filter_stats(const RecoveryContext &ctx)
{
    log_progress(ctx, "DFS filter stats: %d calls, %d candidates evaluated",
                 g_dfs_calls.load(), g_dfs_cands_evaluated.load());
    log_progress(ctx, "  rejected: huff_fail=%d mcu_noinc=%d rst=%d ff00=%d entropy=%d "
                 "boundary=%d dc_bounds=%d mcu_rate=%d oversize=%d",
                 g_filter_huff_fail.load(), g_filter_mcu_noinc.load(),
                 g_filter_rst.load(), g_filter_ff00.load(), g_filter_entropy.load(),
                 g_filter_boundary.load(), g_filter_dc_bounds.load(),
                 g_filter_mcu_rate.load(), g_filter_oversize.load());
    log_progress(ctx, "  accepted: %d", g_cands_accepted.load());
}

ChainResult explore_path(RecoveryContext &ctx, const JpegTemplate &tmpl,
                          const Seed &seed,
                          std::vector<uint32_t> chain_clusters,
                          HuffCheckpoint state, float score_accum,
                          std::vector<BranchPoint> &branches,
                          std::vector<uint8_t> &chain_buf,
                          int max_chain_len)
{
    g_dfs_calls++;
    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;
    auto &cfg = tmpl.mcu_config;
    std::vector<uint32_t> cand_cl(ctx.max_candidates);
    std::vector<float> cand_pri(ctx.max_candidates);

ChainResult chain;
chain.clusters = std::move(chain_clusters);
chain.score = score_accum;
chain.mcus_recovered = state.mcu_count;

std::unordered_set<uint32_t> visited(chain.clusters.begin(), chain.clusters.end());
uint32_t current = chain.clusters.back();

while ((int)chain.clusters.size() < max_chain_len && state.mcu_count < cfg.total_mcus) {
    /* Check if FAT says this is the last cluster (EOF) */
    if (current < ctx.fat.count() &&
        ctx.fat.status[current] == FAT_EOF) {
        chain.complete = true;
        break;
    }

    /* M3: Chain oversize detection (feature flag).
     * If chain has grown past expected size without completing,
     * we're probably following wrong clusters. Stop early. */
    if (FEAT(ctx, oversize_terminate) && seed.expected_size > 0 &&
        chain_buf.size() > (size_t)(seed.expected_size * 1.1) &&
        state.mcu_count < cfg.total_mcus * 9 / 10) {
        g_filter_oversize++;
        log_debug(ctx, "chain oversize: %zu bytes > expected %u, %u/%u MCUs",
                  chain_buf.size(), (uint32_t)(seed.expected_size * 1.1),
                  state.mcu_count, cfg.total_mcus);
        break;
    }

    /* M1: Size-guided EOI check ----
     * When chain reaches expected file size, scan the last cluster
     * for EOI marker. The file should end right around here. */
    if (seed.expected_size > 0 &&
        chain_buf.size() >= seed.expected_size - bpc) {
        /* Look for EOI (FF D9) in the tail of chain_buf */
        size_t search_start = (seed.expected_size > bpc * 2)
            ? seed.expected_size - bpc * 2 : 0;
        if (search_start < chain_buf.size()) {
            for (size_t j = chain_buf.size(); j > search_start && j > 1; j--) {
                if (chain_buf[j-2] == 0xFF && chain_buf[j-1] == 0xD9) {
                    chain_buf.resize(j);
                    chain.complete = true;
                    /* Re-validate to get accurate MCU count */
                    HuffCheckpoint eoi_state = {};
                    if (cfg.restart_interval > 0)
                        eoi_state.mcus_to_restart = cfg.restart_interval;
                    if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
                        eoi_state.scan_ss = cfg.scans[0].ss;
                        eoi_state.scan_se = cfg.scans[0].se;
                        eoi_state.scan_ah = cfg.scans[0].ah;
                        eoi_state.scan_al = cfg.scans[0].al;
                    }
                    size_t hlen = tmpl.header_bytes.size();
                    if (chain_buf.size() > hlen) {
                        huff_validate_cluster(
                            chain_buf.data() + hlen, chain_buf.size() - hlen,
                            cfg, tmpl.dc_tables, tmpl.ac_tables, eoi_state);
                        chain.mcus_recovered = eoi_state.mcu_count;
                        state = eoi_state;
                    }
                    log_debug(ctx, "size-guided EOI at %zu bytes, %u MCUs",
                              chain_buf.size(), state.mcu_count);
                    break;
                }
            }
            if (chain.complete) break;
        }
    }

    /* ---- EOI-guided sequential scan ----
     * When FAT chain is broken, try reading sequential clusters
     * until we hit EOI. This is what foremost does, but validated. */
    bool fat_broken = (current >= ctx.fat.count() ||
                       (ctx.fat.status[current] != FAT_VALID &&
                        ctx.fat.status[current] != FAT_EOF));
    if (fat_broken && seed.expected_clusters > 0) {
        uint32_t seq = current + 1;
        size_t seq_buf_start = chain_buf.size();
        size_t seq_chain_start = chain.clusters.size();
        bool has_eoi = false;
        size_t max_seq = seed.expected_clusters * 1.2;

        while (seq <= ctx.disk.geo.total_clusters + 1 &&
               chain.clusters.size() - seq_chain_start < max_seq) {
            if (visited.count(seq)) { seq++; continue; }
            const uint8_t *sdata = ctx.disk.cluster_ptr(seq);
            if (!sdata) break;

            chain_buf.insert(chain_buf.end(), sdata, sdata + bpc);
            chain.clusters.push_back(seq);
            visited.insert(seq);

            /* Scan for EOI (FF D9) in the newly added cluster.
             * Skip header region to avoid hitting embedded EXIF thumbnail EOI. */
            size_t hdr_end = tmpl.header_bytes.size() + seed.soi_offset;
            size_t dfs_scan_lo = std::max(chain_buf.size() - bpc, hdr_end + 1);
            for (size_t j = chain_buf.size(); j > dfs_scan_lo; j--) {
                if (chain_buf[j-2] == 0xFF && chain_buf[j-1] == 0xD9) {
                    chain_buf.resize(j);
                    has_eoi = true;
                    break;
                }
            }
            seq++;
            if (has_eoi) break;
        }

        /* Validate the sequential extension */
        HuffCheckpoint seq_state = state;
        HuffResult sr = huff_validate_cluster(
            chain_buf.data(), chain_buf.size(),
            cfg, tmpl.dc_tables, tmpl.ac_tables, seq_state);

        if (sr.passed || seq_state.mcu_count > state.mcu_count + 10) {
            state = seq_state;
            current = chain.clusters.back();
            chain.mcus_recovered = state.mcu_count;
            if (has_eoi) chain.complete = true;
            log_debug(ctx, "seq scan: +%zu clusters, %u MCUs, eoi=%d",
                      chain.clusters.size() - seq_chain_start,
                      state.mcu_count, has_eoi);
            if (has_eoi) break;
            continue;
        }

        /* Sequential scan failed - undo */
        while (chain.clusters.size() > seq_chain_start) {
            visited.erase(chain.clusters.back());
            chain.clusters.pop_back();
        }
        chain_buf.resize(seq_buf_start);
    }

    int ncands = enumerate_candidates(
        current, ctx, seed, (int)chain.clusters.size(),
        cand_cl.data(), cand_pri.data(), ctx.max_candidates);

    if (ncands == 0) break;

    HuffCheckpoint saved = state;
    size_t saved_buf_size = chain_buf.size();

    /* Evaluate all candidates */
    struct ScoredCandidate {
        uint32_t cluster;
        float score;
        HuffCheckpoint post_state;
    };
    std::vector<ScoredCandidate> valid;

    for (int i = 0; i < ncands; i++) {
        uint32_t cand = cand_cl[i];
        float pri = cand_pri[i];

        if (visited.count(cand)) continue;
        const uint8_t *cdata = ctx.disk.cluster_ptr(cand);
        if (!cdata) continue;
        g_dfs_cands_evaluated++;

        /* ---- PRUNING FILTERS (cheapest first) ---- */

        /* P5: RST sequence expectancy */
        if (FEAT(ctx, rst_expectancy) && cfg.restart_interval > 0 && saved.mcus_to_restart == 0 &&
            saved.mcu_count > 0 && pri < 100.0f) {
            int expected_rst = saved.rst_counter % 8;
            bool found_rst = false;
            for (size_t j = 0; j + 1 < std::min(bpc, (uint32_t)20); j++) {
                if (cdata[j] == 0xFF && cdata[j+1] >= 0xD0 && cdata[j+1] <= 0xD7) {
                    found_rst = ((cdata[j+1] - 0xD0) == expected_rst);
                    break;
                }
            }
            if (!found_rst) { g_filter_rst++; continue; }
        }

        /* P6: ff00 density check.
         * Only reject candidates with zero ff00 when current has high
         * ff00 - this catches truly non-JPEG data (zeroed clusters,
         * text, etc). Ratio-based filtering is too aggressive because
         * within-file ff00 variation commonly reaches 16-57x. */
        uint32_t cur_idx = current >= 2 ? current - 2 : 0;
        uint32_t cand_idx = cand >= 2 ? cand - 2 : 0;
        if (FEAT(ctx, ff00_prefilter) && pri < 80.0f && cur_idx < ctx.cluster_map.size() && cand_idx < ctx.cluster_map.size()) {
            uint16_t cur_ff = ctx.cluster_map[cur_idx].ff00_count;
            uint16_t cand_ff = ctx.cluster_map[cand_idx].ff00_count;
            /* Only reject if candidate has zero ff00 and current is
             * clearly JPEG entropy data (high ff00). Zero ff00 means
             * the cluster is not JPEG entropy data at all. */
            if (cur_ff > 20 && cand_ff == 0) { g_filter_ff00++; continue; }
        }

        /* P7: Entropy spike detection */
        if (FEAT(ctx, entropy_filter) && pri < 80.0f && cur_idx < ctx.cluster_map.size() && cand_idx < ctx.cluster_map.size()) {
            float cur_ent = ctx.cluster_map[cur_idx].entropy;
            float cand_ent = ctx.cluster_map[cand_idx].entropy;
            if (std::abs(cand_ent - cur_ent) > 0.8f) { g_filter_entropy++; continue; }
        }

        /* First-MCU boundary check (DC coherence) */
        float bc = FEAT(ctx, boundary_check) ? first_mcu_boundary_check(
            cdata, bpc, cfg, tmpl.dc_tables, tmpl.ac_tables, saved) : -1.0f;

        if (bc >= 0.0f && bc < 0.3f && pri < 80.0f) { g_filter_boundary++; continue; }

        /* ---- HUFFMAN VALIDATION via whole-chain buffer ---- */

        /* Append candidate to chain buffer and validate from checkpoint */
        chain_buf.resize(saved_buf_size);
        chain_buf.insert(chain_buf.end(), cdata, cdata + bpc);

        HuffCheckpoint test = saved;
        HuffResult r = huff_validate_cluster(
            chain_buf.data(), chain_buf.size(),
            cfg, tmpl.dc_tables, tmpl.ac_tables, test);

        if (!r.passed) { g_filter_huff_fail++; continue; }

        /* P3: MCU count must increase */
        if (test.mcu_count <= saved.mcu_count && r.error_type == HUFF_OK) { g_filter_mcu_noinc++; continue; }

        /* P4: DC predictor magnitude bounds */
        if (FEAT(ctx, dc_bounds)) {
            bool dc_ok = true;
            for (int c = 0; c < MAX_COMPONENTS; c++) {
                int32_t jump = std::abs(test.dc_pred[c] - saved.dc_pred[c]);
                int32_t threshold = (chain.clusters.size() < 3) ? 2000 : 800;
                if (jump > threshold && pri < 80.0f) {
                    dc_ok = false;
                    break;
                }
            }
            if (!dc_ok) { g_filter_dc_bounds++; continue; }
        }

        /* M2: MCU rate consistency */
        if (FEAT(ctx, mcu_rate_filter) &&
            seed.expected_clusters > 3 && cfg.total_mcus > 0 && pri < 80.0f) {
            float expected_rate = (float)cfg.total_mcus / seed.expected_clusters;
            uint32_t new_mcus = test.mcu_count - saved.mcu_count;
            if (expected_rate > 5.0f) {
                float ratio = (float)new_mcus / expected_rate;
                if (ratio < 0.2f || ratio > 3.0f) { g_filter_mcu_rate++; continue; }
            }
        }

        /* ---- SCORING ---- */
        float s = score_candidate(r, saved, test, cand, current,
                                  seed, ctx.cluster_map.data(), ctx.fat);
        if (bc > 0.0f && pri < 90.0f)
            s *= (0.5f + bc * 0.5f);

        /* M4: MCU progress scoring */
        if (FEAT(ctx, mcu_progress_score) && seed.expected_clusters > 3 && cfg.total_mcus > 0) {
            float mcu_progress = (float)test.mcu_count / cfg.total_mcus;
            float size_progress = (float)(chain.clusters.size() + 1) / seed.expected_clusters;
            if (size_progress > 0.1f) {
                float progress_ratio = mcu_progress / size_progress;
                if (progress_ratio < 0.5f) s *= 0.5f;
            }
        }

        if (s <= 0) continue;

        g_cands_accepted++;
        valid.push_back({cand, s, test});

        /* P1: FAT-confirmed fast accept */
        if (pri >= 100.0f) break;

        /* Early exit on high confidence */
        if (s > ctx.high_confidence && bc > 0.7f) break;
    }

    /* Restore chain_buf (no candidate committed yet) */
    chain_buf.resize(saved_buf_size);

    if (valid.empty()) {
        /* No valid candidate found. Try EOI sequential scan as fallback. */
        if (seed.expected_clusters > 0) {
            uint32_t seq = current + 1;
            size_t seq_buf_start = chain_buf.size();
            size_t seq_chain_start = chain.clusters.size();
            bool has_eoi = false;
            size_t max_seq = seed.expected_clusters * 1.2;

            while (seq <= ctx.disk.geo.total_clusters + 1 &&
                   chain.clusters.size() - seq_chain_start < max_seq) {
                if (visited.count(seq)) { seq++; continue; }
                const uint8_t *sdata = ctx.disk.cluster_ptr(seq);
                if (!sdata) break;

                chain_buf.insert(chain_buf.end(), sdata, sdata + bpc);
                chain.clusters.push_back(seq);
                visited.insert(seq);

                /* Skip header region to avoid embedded EXIF thumbnail EOI */
                size_t hdr2 = tmpl.header_bytes.size() + seed.soi_offset;
                size_t scan_lo2 = std::max(chain_buf.size() - bpc, hdr2 + 1);
                for (size_t j = chain_buf.size(); j > scan_lo2; j--) {
                    if (chain_buf[j-2] == 0xFF && chain_buf[j-1] == 0xD9) {
                        chain_buf.resize(j);
                        has_eoi = true;
                        break;
                    }
                }
                seq++;
                if (has_eoi) break;
            }

            HuffCheckpoint seq_state = state;
            HuffResult sr = huff_validate_cluster(
                chain_buf.data(), chain_buf.size(),
                cfg, tmpl.dc_tables, tmpl.ac_tables, seq_state);

            if (sr.passed || seq_state.mcu_count > state.mcu_count + 10) {
                state = seq_state;
                current = chain.clusters.back();
                chain.mcus_recovered = state.mcu_count;
                if (has_eoi) chain.complete = true;
                log_debug(ctx, "seq scan (fallback): +%zu clusters, %u MCUs, eoi=%d",
                          chain.clusters.size() - seq_chain_start,
                          state.mcu_count, has_eoi);
                if (has_eoi) break;
                continue;
            }

            /* Undo failed sequential scan */
            while (chain.clusters.size() > seq_chain_start) {
                visited.erase(chain.clusters.back());
                chain.clusters.pop_back();
            }
            chain_buf.resize(seq_buf_start);
        }

        /* Bad sector passthrough */
        uint32_t fat_next = (current < ctx.fat.count()) ? ctx.fat.merged[current] : 0;
        if (fat_next >= 2 && fat_next <= ctx.disk.geo.total_clusters + 1) {
            uint32_t fidx = fat_next - 2;
            if (fidx < ctx.cluster_map.size() &&
                (ctx.cluster_map[fidx].content_type == CTYPE_BAD_SECTOR ||
                 ctx.cluster_map[fidx].content_type == CTYPE_EMPTY)) {
                const uint8_t *bd = ctx.disk.cluster_ptr(fat_next);
                if (bd) chain_buf.insert(chain_buf.end(), bd, bd + bpc);
                chain.clusters.push_back(fat_next);
                visited.insert(fat_next);
                for (int c = 0; c < MAX_COMPONENTS; c++)
                    state.dc_pred[c] = 0;
                current = fat_next;
                chain.score -= 0.1f;
                continue;
            }
        }
        break;
    }

    /* Sort by score descending */
    std::sort(valid.begin(), valid.end(),
              [](const ScoredCandidate &a, const ScoredCandidate &b) {
                  return a.score > b.score;
              });

    /* If multiple candidates passed, save branch point for backtracking */
    if (valid.size() > 1) {
        BranchPoint bp;
        bp.step = (int)chain.clusters.size();
        bp.state = saved;
        bp.chain_so_far = chain.clusters;
        bp.score_so_far = chain.score;
        bp.chain_buf_size = saved_buf_size;
        size_t max_alt = (ctx.search_mode == RecoveryContext::SEARCH_FULL)
                       ? valid.size()
                       : (ctx.search_mode == RecoveryContext::SEARCH_BEAM)
                         ? (size_t)ctx.beam_width
                         : 4;
        for (size_t j = 1; j < valid.size() && j < max_alt; j++) {
            bp.candidates.push_back(valid[j].cluster);
            bp.cand_scores.push_back(valid[j].score);
        }
        if (!bp.candidates.empty())
            branches.push_back(std::move(bp));
    }

    /* Take the best candidate - commit its data to chain_buf */
    auto &best = valid[0];
    const uint8_t *best_data = ctx.disk.cluster_ptr(best.cluster);
    if (best_data)
        chain_buf.insert(chain_buf.end(), best_data, best_data + bpc);
    chain.clusters.push_back(best.cluster);
    visited.insert(best.cluster);
    state = best.post_state;
    current = best.cluster;
    chain.score += best.score;
    chain.mcus_recovered = state.mcu_count;

    log_debug(ctx, "step %d: picked %u (score=%.2f, %d alternatives)",
              (int)chain.clusters.size() - 1, best.cluster, best.score,
              (int)valid.size() - 1);

    /* Mark claimed */
    if (best.cluster >= 2 && best.cluster - 2 < (uint32_t)ctx.claimed_score.size()) {
        float avg = chain.score / (float)chain.clusters.size();
        ctx.claimed_score[best.cluster - 2] = avg;
    }

    if (state.mcu_count >= cfg.total_mcus) {
        chain.complete = true;
        break;
    }
}

if (!chain.clusters.empty())
    chain.score /= (float)chain.clusters.size();

return chain;
}
