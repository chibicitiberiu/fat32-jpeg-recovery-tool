/*
 * fast_path.cpp - Fast-path FAT chain recovery
 *
 * When the FAT chain is intact, follow it directly and validate the entire
 * concatenated buffer. Includes chain repair (truncate + sequential),
 * sequential scan fallback, EOI trust, seam detection, and template
 * table substitution.
 */
#include "sdrecov.h"
#include <cstring>
#include <algorithm>

#define FEAT(ctx, flag) ((ctx).features.flag)


/* Helper: check if any cluster in a chain is cross-linked (refcount > 1) */
static bool chain_has_cross_links(const RecoveryContext &ctx,
                                   const std::vector<uint32_t> &chain)
{
    for (uint32_t cl : chain) {
        if (cl < (uint32_t)ctx.cluster_refcount.size() && ctx.cluster_refcount[cl] > 1)
            return true;
    }
    return false;
}

/* Helper: add a chain variant if it has meaningful content and differs from existing */
static void add_variant(std::vector<ChainVariant> &variants,
                         ChainResult chain, const char *tag, float confidence)
{
    if (chain.clusters.size() < 2) return;
    /* Deduplicate: skip if same cluster list already exists */
    for (auto &v : variants) {
        if (v.chain.clusters == chain.clusters) return;
    }
    ChainVariant cv;
    cv.chain = std::move(chain);
    cv.tag = tag;
    cv.confidence = confidence;
    variants.push_back(std::move(cv));
}

FastPathResult try_fast_path(RecoveryContext &ctx, const Seed &seed,
                              const JpegTemplate &tmpl,
                              const std::vector<uint32_t> &header_chain,
                              size_t header_len, bool was_grafted,
                              const std::vector<uint8_t> &graft_header_bytes,
                              size_t graft_entropy_off,
                              uint32_t effective_expected_size,
                              uint32_t estimated_min_size,
                              uint32_t estimated_max_size)
{
    FastPathResult result;
    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;
    auto &cfg = tmpl.mcu_config;
    uint32_t header_chain_len = (uint32_t)((header_len + bpc - 1) / bpc);
    (void)header_chain_len; /* used by seam detection */
/* When the FAT chain is intact (no corrupt entries), follow it to EOF
 * and validate the entire concatenated buffer at once. This avoids
 * the cross-cluster bitstream issue entirely.
 * For greedy: return immediately on success.
 * For beam/full: save as baseline, then try DFS to improve. */
{
    std::vector<uint32_t> fat_chain;
    bool fat_chain_broken = false;
    bool fat_chain_premature_eof = false;
    bool fat_chain_was_sequential = true;
    uint32_t cl = seed.start_cluster;
    while (cl >= 2 && cl <= ctx.disk.geo.total_clusters + 1 &&
           fat_chain.size() < 10000) {
        fat_chain.push_back(cl);
        if (cl >= ctx.fat.count()) break;
        if (ctx.fat.status[cl] == FAT_EOF) {
            fat_chain_premature_eof = true; /* might be real EOF or corrupted */
            break;
        }
        if (ctx.fat.status[cl] != FAT_VALID) {
            fat_chain_broken = true;
            break;
        }
        uint32_t next = ctx.fat.merged[cl];
        if (next == cl) break; /* loop */

        /* Detect large jumps in otherwise-sequential chains.
         * A FAT cross-link typically redirects to a distant cluster.
         * If the chain has been sequential so far and the next pointer
         * jumps far away, treat this as a break rather than following. */
        if (fat_chain_was_sequential && fat_chain.size() > 1) {
            if (fat_chain[fat_chain.size()-1] == fat_chain[fat_chain.size()-2] + 1 &&
                next != cl + 1) {
                /* Chain was sequential but FAT now jumps away */
                int64_t jump = (int64_t)next - (int64_t)cl;
                if (std::abs(jump) > 100) {
                    log_detail(ctx, "seed %u: FAT chain jumped %+lld at cluster %u "
                               "(was sequential), treating as break",
                               seed.start_cluster, (long long)jump, cl);
                    fat_chain_broken = true;
                    break;
                }
            }
            if (next != cl + 1) fat_chain_was_sequential = false;
        }

        cl = next;
    }

    /* Try extending sequentially when:
     * - FAT chain broke (corrupt entry)
     * - Premature EOF and chain is suspiciously short for expected file size
     * The data might be contiguous on disk. */
    bool should_extend = fat_chain_broken;
    if (fat_chain_premature_eof && !fat_chain.empty()) {
        size_t chain_bytes = fat_chain.size() * bpc;
        /* Check against dir-entry expected size */
        if (seed.expected_size > 0 && chain_bytes < seed.expected_size * 9 / 10)
            should_extend = true;
        /* For sig-scan seeds without expected_size: use image dimensions
         * from the parsed JPEG header to estimate minimum file size.
         * Typical JPEG: 10-30 bytes per MCU. Use 6 as a conservative floor
         * (extremely compressed files). If the chain is shorter than this,
         * the EOF was almost certainly premature. */
        if (seed.expected_size == 0 && cfg.total_mcus > 0) {
            size_t estimated_min = (size_t)cfg.total_mcus * 6 + header_len;
            if (chain_bytes < estimated_min)
                should_extend = true;
        }
        /* Also extend very short chains */
        if (seed.expected_size == 0 && fat_chain.size() < 5)
            should_extend = true;
    }

    if (should_extend && !fat_chain.empty()) {
        uint32_t last = fat_chain.back();
        uint32_t sq = last + 1;
        size_t max_extend = seed.expected_size > 0
            ? (seed.expected_size / bpc + 2) : 10000;
        size_t pre_extend = fat_chain.size();

        while (sq <= ctx.disk.geo.total_clusters + 1 &&
               fat_chain.size() < max_extend) {
            const uint8_t *sd = ctx.disk.cluster_ptr(sq);
            if (!sd) break;

            /* Stop if this cluster looks like a new JPEG file header */
            if (sd[0] == 0xFF && sd[1] == 0xD8 && sd[2] == 0xFF)
                break;

            fat_chain.push_back(sq);

            /* Check for EOI in this cluster (past header region) */
            size_t byte_pos = fat_chain.size() * bpc;
            if (byte_pos > header_len + bpc) {
                for (size_t j = bpc; j > 1; j--) {
                    if (sd[j-2] == 0xFF && sd[j-1] == 0xD9) {
                        goto done_extending;
                    }
                }
            }

            sq++;
        }
        done_extending:
        if (fat_chain.size() > pre_extend) {
            log_detail(ctx, "seed %u: extended %s chain %zu->%zu clusters",
                       seed.start_cluster,
                       fat_chain_broken ? "broken" : "premature-EOF",
                       pre_extend, fat_chain.size());
        }
    }

    if (fat_chain.size() >= 2) {
        /* Concatenate all clusters */
        std::vector<uint8_t> file_data;
        file_data.reserve(fat_chain.size() * bpc);
        for (uint32_t fc : fat_chain) {
            const uint8_t *d = ctx.disk.cluster_ptr(fc);
            if (!d) break;
            file_data.insert(file_data.end(), d, d + bpc);
        }

        /* Trim to expected size if known */
        if (seed.expected_size > 0 && seed.expected_size < file_data.size())
            file_data.resize(seed.expected_size);

        /* Scan for EOI if no expected size.
         * Search backward from end but only within entropy data - don't
         * go into the header region, which contains EXIF thumbnails with
         * their own FF D9 markers that would cause false truncation. */
        if (seed.expected_size == 0 && file_data.size() > header_len + 2) {
            for (size_t j = file_data.size(); j > header_len + 1; j--) {
                if (file_data[j-2] == 0xFF && file_data[j-1] == 0xD9) {
                    file_data.resize(j);
                    break;
                }
            }
        }

        /* Incremental DC validation: walk cluster-by-cluster and detect
         * DC discontinuities that indicate cross-linked clusters.
         * Unlike the post-hoc seam detection (which only runs on validated
         * chains), this runs early to detect cross-links BEFORE we decide
         * whether to try sequential. A DC jump > 500 at a non-sequential
         * FAT transition is strong evidence of a cross-link. */
        int dc_seam_at = -1;
        if (file_data.size() > header_len && fat_chain.size() > header_chain_len + 2) {
            HuffCheckpoint dc_walk = {};
            if (cfg.restart_interval > 0)
                dc_walk.mcus_to_restart = cfg.restart_interval;
            if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
                dc_walk.scan_ss = cfg.scans[0].ss;
                dc_walk.scan_se = cfg.scans[0].se;
                dc_walk.scan_ah = cfg.scans[0].ah;
                dc_walk.scan_al = cfg.scans[0].al;
            }

            size_t dc_buf_pos = 0;
            for (size_t ci = 0; ci < fat_chain.size() && dc_seam_at < 0; ci++) {
                int32_t dc_before[MAX_COMPONENTS];
                for (int c = 0; c < MAX_COMPONENTS; c++)
                    dc_before[c] = dc_walk.dc_pred[c];
                uint32_t mcus_before = dc_walk.mcu_count;

                dc_buf_pos += bpc;
                size_t check_len = std::min(dc_buf_pos, file_data.size());
                HuffCheckpoint dc_test = dc_walk;
                huff_validate_cluster(file_data.data(), check_len,
                    cfg, tmpl.dc_tables, tmpl.ac_tables, dc_test);

                if (ci > header_chain_len && dc_test.mcu_count > mcus_before) {
                    float max_jump = 0;
                    for (int c = 0; c < cfg.num_components; c++) {
                        float jump = std::abs((float)(dc_test.dc_pred[c] - dc_before[c]));
                        if (jump > max_jump) max_jump = jump;
                    }

                    /* Check if this is a non-sequential FAT transition */
                    bool is_non_sequential = (ci > 0 &&
                        fat_chain[ci] != fat_chain[ci-1] + 1);

                    if (max_jump > 500.0f ||
                        (max_jump > 200.0f && is_non_sequential)) {
                        dc_seam_at = (int)ci;
                        log_detail(ctx, "seed %u: DC seam at cluster %zu "
                                   "(jump=%.0f, non_seq=%d), will try sequential",
                                   seed.start_cluster, ci, max_jump,
                                   is_non_sequential);
                    }
                }
                dc_walk = dc_test;
            }
        }

        /* Validate the whole buffer.
         * Use a fresh checkpoint since we're re-processing all entropy
         * data from the start (init_state has partial MCU counts from
         * the header cluster's tail validation). */
        if (file_data.size() > header_len) {
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

            log_debug(ctx, "seed %u: fast-path validate: passed=%d mcus=%u/%u err=%d chainlen=%zu",
                     seed.start_cluster, vr.passed, vr.mcu_count, cfg.total_mcus,
                     vr.error_type, fat_chain.size());

            /* If fast-path partially failed, try repairing: keep the good
             * prefix (up to the failure cluster), then continue sequentially.
             * This handles the case where FAT points to wrong clusters but
             * the correct data is sequential on disk. */
            if (FEAT(ctx, chain_repair) && !vr.passed && vr.mcu_count > 0 && vr.offset > 0) {
                size_t fail_byte = header_len + vr.offset;
                size_t fail_cluster_idx = fail_byte / bpc;
                if (fail_cluster_idx > 0 && fail_cluster_idx < fat_chain.size()) {
                    /* Truncate to good prefix */
                    std::vector<uint32_t> repaired_chain(
                        fat_chain.begin(), fat_chain.begin() + fail_cluster_idx);
                    uint32_t last_good = repaired_chain.back();

                    /* Continue sequentially from last good cluster */
                    uint32_t seq = last_good + 1;
                    size_t max_bytes = seed.expected_size > 0
                        ? (size_t)(seed.expected_size * 1.2)
                        : fat_chain.size() * bpc;

                    file_data.resize(fail_cluster_idx * bpc);
                    bool found_eoi = false;

                    while (seq <= ctx.disk.geo.total_clusters + 1 &&
                           file_data.size() < max_bytes) {
                        const uint8_t *sd = ctx.disk.cluster_ptr(seq);
                        if (!sd) break;
                        repaired_chain.push_back(seq);
                        file_data.insert(file_data.end(), sd, sd + bpc);

                        /* Check for EOI in this cluster (skip header region) */
                        size_t scan_start = std::max(file_data.size() - bpc,
                                                      header_len + 1);
                        for (size_t j = file_data.size(); j > scan_start; j--) {
                            if (file_data[j-2] == 0xFF && file_data[j-1] == 0xD9) {
                                file_data.resize(j);
                                found_eoi = true;
                                break;
                            }
                        }
                        if (found_eoi) break;
                        seq++;
                    }

                    /* Validate the repaired chain */
                    if (file_data.size() > header_len) {
                        HuffCheckpoint rstate = {};
                        if (cfg.restart_interval > 0)
                            rstate.mcus_to_restart = cfg.restart_interval;
                        if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
                            rstate.scan_ss = cfg.scans[0].ss;
                            rstate.scan_se = cfg.scans[0].se;
                            rstate.scan_ah = cfg.scans[0].ah;
                            rstate.scan_al = cfg.scans[0].al;
                        }
                        HuffResult rr = huff_validate_cluster(
                            file_data.data() + header_len,
                            file_data.size() - header_len,
                            cfg, tmpl.dc_tables, tmpl.ac_tables, rstate);

                        if (rr.mcu_count > vr.mcu_count) {
                            log_detail(ctx, "seed %u: repaired chain at cluster %zu: "
                                       "%u->%u MCUs (FAT prefix + sequential)",
                                       seed.start_cluster, fail_cluster_idx,
                                       vr.mcu_count, rr.mcu_count);
                            vr = rr;
                            fat_chain = std::move(repaired_chain);
                        }
                    }
                }
            }

            /* Also try fully sequential scan (ignore FAT entirely).
             * Like foremost: read consecutive clusters from start.
             * Compare against FAT chain and pick whichever has more MCUs.
             * Try when: validation failed OR partially validated (<90% MCUs)
             * OR cross-links detected OR low FAT confidence. */
            bool has_xlinks = chain_has_cross_links(ctx, fat_chain);
            bool has_dc_seam = (dc_seam_at >= 0);
            /* Check if FAT chain deviates from sequential order */
            bool fat_non_sequential = false;
            for (size_t ci = 1; ci < fat_chain.size(); ci++) {
                if (fat_chain[ci] != fat_chain[ci-1] + 1) {
                    fat_non_sequential = true;
                    break;
                }
            }

            /* Flag chain as suspect when we have specific evidence:
             * - cross-linked clusters (refcount > 1)
             * - DC discontinuity at cluster boundary
             * - FAT chain deviates from sequential AND FAT has corruption */
            bool suspect_chain = has_xlinks || has_dc_seam ||
                (fat_non_sequential && ctx.fat_confidence < 1.0f);
            if (suspect_chain) {
                log_detail(ctx, "seed %u: suspect FAT chain (xlinks=%d dc_seam=%d "
                           "non_seq=%d fat_conf=%.1f%%), will try sequential",
                           seed.start_cluster, has_xlinks, has_dc_seam,
                           fat_non_sequential, ctx.fat_confidence * 100);
            }
            bool try_sequential = FEAT(ctx, sequential_scan) && (!vr.passed ||
                (cfg.total_mcus > 0 && vr.mcu_count < cfg.total_mcus * 9 / 10) ||
                (seed.expected_size > 0 && file_data.size() < seed.expected_size / 2) ||
                (estimated_min_size > 0 && file_data.size() < estimated_min_size / 2) ||
                suspect_chain);
            if (try_sequential) {
                std::vector<uint32_t> seq_chain;
                std::vector<uint8_t> seq_data;
                size_t max_bytes = effective_expected_size > 0
                    ? (size_t)(effective_expected_size * 1.2)
                    : fat_chain.size() * bpc;
                uint32_t sq = seed.start_cluster;
                bool seq_eoi = false;

                while (sq <= ctx.disk.geo.total_clusters + 1 &&
                       seq_data.size() < max_bytes) {
                    const uint8_t *sd = ctx.disk.cluster_ptr(sq);
                    if (!sd) break;
                    seq_chain.push_back(sq);
                    seq_data.insert(seq_data.end(), sd, sd + bpc);
                    /* Check for EOI in this cluster (skip header region) */
                    size_t seq_scan_start = std::max(seq_data.size() - bpc,
                                                      header_len + 1);
                    for (size_t j = seq_data.size(); j > seq_scan_start; j--) {
                        if (seq_data[j-2] == 0xFF && seq_data[j-1] == 0xD9) {
                            seq_data.resize(j);
                            seq_eoi = true;
                            break;
                        }
                    }
                    if (seq_eoi) break;
                    sq++;
                }

                if (seq_data.size() > header_len) {
                    HuffCheckpoint sstate = {};
                    if (cfg.restart_interval > 0)
                        sstate.mcus_to_restart = cfg.restart_interval;
                    if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
                        sstate.scan_ss = cfg.scans[0].ss;
                        sstate.scan_se = cfg.scans[0].se;
                        sstate.scan_ah = cfg.scans[0].ah;
                        sstate.scan_al = cfg.scans[0].al;
                    }
                    HuffResult sr = huff_validate_cluster(
                        seq_data.data() + header_len,
                        seq_data.size() - header_len,
                        cfg, tmpl.dc_tables, tmpl.ac_tables, sstate);

                    bool suspect_fat = suspect_chain;
                    if (sr.mcu_count > vr.mcu_count) {
                        /* Save the FAT chain as an alternative before replacing */
                        if (suspect_fat && vr.mcu_count > 0 && fat_chain != seq_chain) {
                            ChainResult fat_variant;
                            fat_variant.clusters = fat_chain;
                            fat_variant.mcus_recovered = vr.mcu_count;
                            fat_variant.total_mcus = cfg.total_mcus;
                            fat_variant.score = 0.90f;
                            float fat_conf = cfg.total_mcus > 0
                                ? (float)vr.mcu_count / cfg.total_mcus * 0.8f : 0.5f;
                            add_variant(result.variants, std::move(fat_variant),
                                        "fat", fat_conf);
                        }
                        log_detail(ctx, "seed %u: sequential scan better: %u->%u MCUs",
                                   seed.start_cluster, vr.mcu_count, sr.mcu_count);
                        vr = sr;
                        fat_chain = std::move(seq_chain);
                        file_data = std::move(seq_data);
                    } else if (suspect_fat && sr.mcu_count > 0 && fat_chain != seq_chain) {
                        /* Sequential wasn't better overall but differs and has merit -
                         * save it as an alternative variant */
                        ChainResult seq_variant;
                        seq_variant.clusters = seq_chain;
                        seq_variant.mcus_recovered = sr.mcu_count;
                        seq_variant.total_mcus = cfg.total_mcus;
                        seq_variant.score = 0.85f;
                        float seq_conf = cfg.total_mcus > 0
                            ? (float)sr.mcu_count / cfg.total_mcus * 0.7f : 0.4f;
                        add_variant(result.variants, std::move(seq_variant),
                                    "sequential", seq_conf);
                    }

                    /* If sequential found EOI near expected size and validates
                     * at least some MCUs, trust it. Two strong signals:
                     * 1. EOI marker at the right file size = correct data
                     * 2. Some MCUs validate = correct JPEG header/tables
                     * Bit errors cause early validation failure but the
                     * file data is there and a viewer can display it. */
                    if (seq_eoi && sr.mcu_count > 0) {
                        /* Trust EOI if file size falls within a plausible range.
                         * For dir-entry seeds: within 80-120% of known size.
                         * For sig-scan seeds: within estimated min-max range. */
                        bool size_ok = false;
                        if (seed.expected_size > 0) {
                            float r = (float)seq_data.size() / seed.expected_size;
                            size_ok = (r > 0.8f && r < 1.2f);
                        } else if (estimated_min_size > 0) {
                            size_ok = (seq_data.size() >= estimated_min_size &&
                                       seq_data.size() <= estimated_max_size);
                        }
                        if (size_ok) {
                            log_detail(ctx, "seed %u: sequential EOI in plausible range "
                                       "(%zu bytes, %u MCUs), accepting",
                                       seed.start_cluster, seq_data.size(),
                                       sr.mcu_count);
                            vr.passed = true;
                            vr.mcu_count = std::max(vr.mcu_count, sr.mcu_count);
                            fat_chain = std::move(seq_chain);
                            file_data = std::move(seq_data);
                        }
                    }
                }
            }

            /* Table substitution: if validation is poor, try alternative
             * DHT tables. Bit errors in the file's header can corrupt
             * Huffman tables while leaving entropy data intact.
             *
             * Strategy (from research):
             * 1. Standard Annex K tables (~90% of cameras use these)
             * 2. Template library tables (camera-specific) */
            int best_template_idx = -1;
            if (FEAT(ctx, template_tables) && !vr.passed && vr.mcu_count < cfg.total_mcus / 2 &&
                file_data.size() > header_len) {

                auto try_tables = [&](const HuffTable dc_tbl[], const HuffTable ac_tbl[],
                                      const char *label) -> bool {
                    HuffCheckpoint tstate = {};
                    if (cfg.restart_interval > 0)
                        tstate.mcus_to_restart = cfg.restart_interval;
                    if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
                        tstate.scan_ss = cfg.scans[0].ss;
                        tstate.scan_se = cfg.scans[0].se;
                        tstate.scan_ah = cfg.scans[0].ah;
                        tstate.scan_al = cfg.scans[0].al;
                    }
                    HuffResult tr = huff_validate_cluster(
                        file_data.data() + header_len,
                        file_data.size() - header_len,
                        cfg, dc_tbl, ac_tbl, tstate);
                    if (tr.mcu_count > vr.mcu_count * 2 ||
                        (tr.passed && !vr.passed)) {
                        log_detail(ctx, "seed %u: %s tables improved: %u->%u MCUs",
                                   seed.start_cluster, label,
                                   vr.mcu_count, tr.mcu_count);
                        vr = tr;
                        return true;
                    }
                    return false;
                };

                /* 1. Try standard Annex K tables first (covers ~90% of cameras) */
                build_standard_tables();
                if (try_tables(g_std_dc, g_std_ac, "standard Annex K")) {
                    best_template_idx = -2; /* sentinel: standard tables used */
                }

                /* 2. Try template library tables */
                if (best_template_idx == -1) {
                    for (int t = 0; t < (int)ctx.templates.size(); t++) {
                        auto &tpl = ctx.templates[t];
                        if (tpl.mcu_config.num_components != cfg.num_components) continue;
                        if (tpl.mcu_config.blocks_per_mcu != cfg.blocks_per_mcu) continue;

                        char label[64];
                        snprintf(label, sizeof(label), "template %d (%dx%d)",
                                 t, tpl.width, tpl.height);
                        if (try_tables(tpl.dc_tables, tpl.ac_tables, label)) {
                            best_template_idx = t;
                            if (vr.passed) break;
                        }
                    }
                }
            }

            if (vr.passed || vr.mcu_count > cfg.total_mcus / 2) {
                /* Good chain - build into a local ChainResult */
                ChainResult primary;
                primary.clusters = std::move(fat_chain);
                primary.mcus_recovered = vr.mcu_count;
                primary.total_mcus = cfg.total_mcus;
                if (!vr.passed && cfg.total_mcus > 0 &&
                    vr.mcu_count < cfg.total_mcus * 95 / 100)
                    primary.has_validation_gap = true;
                primary.complete = (ctx.fat.status[primary.clusters.back()] == FAT_EOF);
                primary.score = 0.95f;

                /* Mark claimed */
                for (uint32_t fc : primary.clusters) {
                    if (fc >= 2 && fc - 2 < (uint32_t)ctx.claimed_score.size())
                        ctx.claimed_score[fc - 2] = 0.95f;
                }

                log_detail(ctx, "seed %u: fast-path FAT chain (%zu clusters, %u MCUs, %s)",
                           seed.start_cluster, primary.clusters.size(),
                           vr.mcu_count, primary.complete ? "complete" : "partial");

                if (best_template_idx >= 0) {
                    /* Template tables improved validation. Don't graft
                     * the template header (wrong dimensions/DQT). The
                     * original header with minor DHT bit errors is close
                     * enough for most viewers, and preserves EXIF/DQT/SOF. */
                } else if (was_grafted) {
                    primary.grafted = true;
                    primary.graft_header = graft_header_bytes;
                    primary.entropy_offset = graft_entropy_off;
                }

                /* Return immediately only if fully validated AND chain
                 * covers a reasonable portion of the expected file.
                 * A truncated FAT chain (e.g. 9 clusters for a 13MB file)
                 * can pass validation cleanly (EOF, not error) but is not
                 * a good recovery. Fall through to sequential scan. */
                bool chain_adequate = true;
                if (FEAT(ctx, chain_adequacy)) {
                    if (seed.expected_size > 0) {
                        /* Known size: chain should be at least 50% */
                        if (file_data.size() < seed.expected_size / 2)
                            chain_adequate = false;
                    } else if (estimated_min_size > 0) {
                        /* Estimated range: chain should be at least the minimum
                         * plausible compressed size */
                        if (file_data.size() < estimated_min_size / 2)
                            chain_adequate = false;
                    }
                    if (cfg.total_mcus > 0 && vr.mcu_count < cfg.total_mcus / 2)
                        chain_adequate = false;
                }

                /* Post-hoc seam detection */
                if (FEAT(ctx, seam_detection) &&
                    primary.clusters.size() > 2 &&
                    cfg.total_mcus > 0 && vr.mcu_count > 0) {
                    /* Decode cluster by cluster, checking DC at each boundary */
                    HuffCheckpoint walk_state = {};
                    if (cfg.restart_interval > 0)
                        walk_state.mcus_to_restart = cfg.restart_interval;
                    if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
                        walk_state.scan_ss = cfg.scans[0].ss;
                        walk_state.scan_se = cfg.scans[0].se;
                        walk_state.scan_ah = cfg.scans[0].ah;
                        walk_state.scan_al = cfg.scans[0].al;
                    }

                    size_t walk_buf_size = 0;
                    int seam_at = -1;

                    for (size_t ci = 0; ci < primary.clusters.size(); ci++) {
                        const uint8_t *cd = ctx.disk.cluster_ptr(primary.clusters[ci]);
                        if (!cd) break;

                        /* Save DC state before this cluster */
                        int32_t dc_before[MAX_COMPONENTS];
                        for (int c = 0; c < MAX_COMPONENTS; c++)
                            dc_before[c] = walk_state.dc_pred[c];
                        uint32_t mcus_before = walk_state.mcu_count;

                        walk_buf_size += bpc;
                        /* Validate up through this cluster */
                        HuffCheckpoint test_walk = walk_state;
                        huff_validate_cluster(
                            file_data.data(), walk_buf_size > file_data.size() ? file_data.size() : walk_buf_size,
                            cfg, tmpl.dc_tables, tmpl.ac_tables, test_walk);

                        /* Skip header clusters and first data cluster */
                        if (ci <= header_chain_len || test_walk.mcu_count <= mcus_before)
                            { walk_state = test_walk; continue; }

                        /* Check DC discontinuity at this boundary */
                        float max_jump = 0;
                        for (int c = 0; c < cfg.num_components; c++) {
                            float jump = std::abs((float)(test_walk.dc_pred[c] - dc_before[c]));
                            if (jump > max_jump) max_jump = jump;
                        }

                        /* A jump > 500 is a strong seam signal.
                         * Normal same-image transitions rarely exceed 200. */
                        if (max_jump > 500.0f && ci > header_chain_len + 1) {
                            seam_at = (int)ci;
                            log_detail(ctx, "seed %u: seam detected at cluster %zu "
                                       "(DC jump %.0f), truncating and retrying sequential",
                                       seed.start_cluster, ci, max_jump);
                            break;
                        }
                        walk_state = test_walk;
                    }

                    if (seam_at > 0) {
                        /* Truncate chain at seam, rebuild file_data, try sequential */
                        std::vector<uint32_t> good_prefix(
                            primary.clusters.begin(),
                            primary.clusters.begin() + seam_at);
                        uint32_t last_good = good_prefix.back();

                        file_data.resize(seam_at * bpc);
                        if (seed.expected_size > 0 && seed.expected_size < file_data.size())
                            file_data.resize(seed.expected_size);

                        /* Continue sequentially from last good cluster */
                        uint32_t sq = last_good + 1;
                        size_t max_bytes = effective_expected_size > 0
                            ? (size_t)(effective_expected_size * 1.2)
                            : primary.clusters.size() * bpc;
                        bool found_eoi = false;

                        while (sq <= ctx.disk.geo.total_clusters + 1 &&
                               file_data.size() < max_bytes) {
                            const uint8_t *sd = ctx.disk.cluster_ptr(sq);
                            if (!sd) break;
                            good_prefix.push_back(sq);
                            file_data.insert(file_data.end(), sd, sd + bpc);
                            /* Check for EOI in this cluster (skip header region) */
                            size_t seam_scan_start = std::max(file_data.size() - bpc,
                                                               header_len + 1);
                            for (size_t j = file_data.size(); j > seam_scan_start; j--) {
                                if (file_data[j-2] == 0xFF && file_data[j-1] == 0xD9) {
                                    file_data.resize(j);
                                    found_eoi = true;
                                    break;
                                }
                            }
                            if (found_eoi) break;
                            sq++;
                        }

                        /* Re-validate the repaired chain */
                        HuffCheckpoint rstate = {};
                        if (cfg.restart_interval > 0)
                            rstate.mcus_to_restart = cfg.restart_interval;
                        if (cfg.jpeg_mode == JPEG_PROGRESSIVE && cfg.num_scans > 0) {
                            rstate.scan_ss = cfg.scans[0].ss;
                            rstate.scan_se = cfg.scans[0].se;
                            rstate.scan_ah = cfg.scans[0].ah;
                            rstate.scan_al = cfg.scans[0].al;
                        }
                        if (file_data.size() > header_len) {
                            HuffResult rr = huff_validate_cluster(
                                file_data.data() + header_len,
                                file_data.size() - header_len,
                                cfg, tmpl.dc_tables, tmpl.ac_tables, rstate);
                            if (rr.mcu_count > vr.mcu_count ||
                                (found_eoi && rr.mcu_count > 0)) {
                                log_detail(ctx, "seed %u: seam repair: %u->%u MCUs, eoi=%d",
                                           seed.start_cluster, vr.mcu_count,
                                           rr.mcu_count, found_eoi);
                                vr = rr;
                                fat_chain = std::move(good_prefix);
                                primary.clusters = fat_chain;
                                primary.mcus_recovered = rr.mcu_count;
                                primary.complete = found_eoi;
                            }
                        }
                    }
                }

                float this_eval = evaluate_chain(ctx, seed, primary);

                /* Save the primary chain as a variant */
                {
                    float conf = cfg.total_mcus > 0
                        ? (float)vr.mcu_count / cfg.total_mcus : 0.5f;
                    if (vr.passed) conf = std::min(conf + 0.1f, 1.0f);
                    /* Cap confidence when cross-links or DC seam detected -
                     * we can't trust the FAT chain even if it validates
                     * perfectly, because the data might be from another file */
                    if (suspect_chain)
                        conf = std::min(conf, 0.75f);
                    add_variant(result.variants, primary, "fat", conf);
                    result.best_eval = this_eval;
                }

                /* Early return in greedy mode when chain is good.
                 * Even with cross-links: fast-path already collected both FAT
                 * and sequential variants above. DFS won't help since it
                 * follows the same FAT chain. */
                if (ctx.search_mode == RecoveryContext::SEARCH_GREEDY &&
                    vr.passed && chain_adequate) {
                    result.should_return = true;
                    return result;
                }
            }
        }
    }
}


    return result;
}
