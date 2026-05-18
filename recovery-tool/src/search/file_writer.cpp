/*
 * file_writer.cpp - Write recovered JPEG files to disk
 */
#include "sdrecov.h"
#include <cstring>

/*
 * Try to find a resync point in entropy data after a corruption gap.
 * Scans forward from fail_offset, tries decoding from each byte.
 * Returns the offset of the best resync point, or 0 if none found.
 */
static size_t find_resync_point(const uint8_t *entropy, size_t entropy_len,
                                 size_t fail_offset, uint32_t bpc,
                                 const McuConfig &cfg,
                                 const HuffTable dc_tables[],
                                 const HuffTable ac_tables[],
                                 uint32_t &out_mcus)
{
    size_t best_offset = 0;
    uint32_t best_mcus = 0;

    /* Search up to 2 clusters ahead for a resync point */
    size_t search_limit = std::min(fail_offset + bpc * 2, entropy_len);
    for (size_t try_off = fail_offset + 1; try_off < search_limit; try_off++) {
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
            entropy + try_off, entropy_len - try_off,
            cfg, dc_tables, ac_tables, rstate);
        if (rr.mcu_count > best_mcus) {
            best_mcus = rr.mcu_count;
            best_offset = try_off;
        }
        if (rr.mcu_count > 10) break; /* good enough */
    }

    out_mcus = best_mcus;
    return best_offset;
}

bool write_recovered_with_rst(const RecoveryContext &ctx, const Seed &seed,
                                      const ChainResult &chain, int file_num)
{
    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;

    /* Build the complete file data */
    std::vector<uint8_t> file_data;
    for (size_t i = 0; i < chain.clusters.size(); i++) {
        const uint8_t *d = ctx.disk.cluster_ptr(chain.clusters[i]);
        if (d)
            file_data.insert(file_data.end(), d, d + bpc);
        else
            file_data.insert(file_data.end(), bpc, 0);
    }

    /* Parse header */
    size_t soi_off = seed.soi_offset;
    JpegTemplate tmpl;
    if (!jpeg_parse_header(file_data.data() + soi_off,
                            file_data.size() - soi_off, tmpl))
        return false;

    auto &cfg = tmpl.mcu_config;
    size_t header_len = soi_off + tmpl.header_bytes.size();
    if (file_data.size() <= header_len) return false;
    /* Skip progressive JPEGs - RST injection for multi-scan is complex */
    if (cfg.jpeg_mode == JPEG_PROGRESSIVE) return false;

    const uint8_t *entropy = file_data.data() + header_len;
    size_t entropy_len = file_data.size() - header_len;

    /* If file already has DRI, use original header. Otherwise inject synthetic DRI
     * with restart_interval = mcu_width (one row of MCUs). */
    bool synthetic_dri = (cfg.restart_interval == 0);
    uint16_t rst_interval = cfg.restart_interval;
    std::vector<uint8_t> modified_header;

    if (synthetic_dri) {
        rst_interval = cfg.mcu_width;
        if (rst_interval == 0) rst_interval = 1;
        modified_header = inject_dri(tmpl.header_bytes, rst_interval);
    }

    const std::vector<uint8_t> &out_header = synthetic_dri
        ? modified_header : tmpl.header_bytes;

    /* Find all gaps by iterating through decode-fail-resync cycles */
    struct GapInfo {
        size_t fail_offset;
        size_t resync_offset;
        uint32_t pre_mcus;
        uint32_t post_mcus;
        int32_t dc_before[MAX_COMPONENTS];
    };

    std::vector<GapInfo> gaps;
    uint32_t total_recovered_mcus = 0;
    size_t scan_pos = 0;

    while (scan_pos < entropy_len && gaps.size() < 20) {
        HuffCheckpoint state = {};
        state.mcus_to_restart = rst_interval;

        HuffResult vr = huff_validate_cluster(
            entropy + scan_pos, entropy_len - scan_pos,
            cfg, tmpl.dc_tables, tmpl.ac_tables, state);

        total_recovered_mcus += vr.mcu_count;

        if (vr.passed) break; /* reached end cleanly */
        if (vr.offset == 0 && vr.mcu_count == 0) break; /* can't decode anything */

        GapInfo gap;
        gap.fail_offset = scan_pos + vr.offset;
        gap.pre_mcus = vr.mcu_count;
        for (int c = 0; c < MAX_COMPONENTS; c++)
            gap.dc_before[c] = state.dc_pred[c];

        /* Find resync point after the gap */
        uint32_t resync_mcus = 0;
        size_t resync = find_resync_point(entropy, entropy_len,
            gap.fail_offset, bpc, cfg,
            tmpl.dc_tables, tmpl.ac_tables, resync_mcus);

        if (resync == 0 || resync_mcus < 3) break; /* can't resync */

        gap.resync_offset = resync;
        gap.post_mcus = resync_mcus;
        gaps.push_back(gap);

        scan_pos = resync;
    }

    if (gaps.empty()) return false;

    /* Need meaningful recovery: pre-gap MCUs + post-gap MCUs > some threshold */
    uint32_t post_gap_total = 0;
    for (auto &g : gaps) post_gap_total += g.post_mcus;
    if (total_recovered_mcus + post_gap_total < cfg.total_mcus / 4)
        return false; /* too little data to be useful */

    /* Build output: header + entropy with RST markers at gap boundaries */
    std::vector<uint8_t> output;
    output.insert(output.end(), out_header.begin(), out_header.end());

    size_t write_pos = 0;
    int rst_num = 0;

    for (auto &gap : gaps) {
        /* Write good entropy up to the failure point */
        if (gap.fail_offset > write_pos)
            output.insert(output.end(),
                          entropy + write_pos, entropy + gap.fail_offset);

        /* Pad to RST boundary: fill remaining MCUs in current restart interval
         * with minimal valid data (DC=0, all AC=0 via EOB).
         * This is needed so the decoder reaches the RST marker at the right MCU count. */

        /* Insert RST marker */
        output.push_back(0xFF);
        output.push_back(0xD0 + (rst_num % 8));
        rst_num++;

        /* Resume from resync point */
        write_pos = gap.resync_offset;
    }

    /* Write remaining entropy after last gap */
    if (write_pos < entropy_len)
        output.insert(output.end(), entropy + write_pos, entropy + entropy_len);

    /* Ensure EOI */
    if (output.size() >= 2 &&
        !(output[output.size()-2] == 0xFF && output[output.size()-1] == 0xD9)) {
        output.push_back(0xFF);
        output.push_back(0xD9);
    }

    /* Write to file */
    const char *basename = seed.filename.c_str();
    const char *slash = strrchr(basename, '/');
    if (slash) basename = slash + 1;

    char path[1024];
    if (basename[0] && seed.source != SEED_SIGNATURE_SCAN)
        snprintf(path, sizeof(path), "%s/files/%04d_%s", ctx.output_dir.c_str(), file_num, basename);
    else
        snprintf(path, sizeof(path), "%s/files/%04d_cluster_%u.jpg",
                 ctx.output_dir.c_str(), file_num, seed.start_cluster);

    FILE *f = fopen(path, "wb");
    if (!f) return false;
    fwrite(output.data(), 1, output.size(), f);
    fclose(f);

    log_info(ctx, "recovered (RST-injected, %zu gaps, %s DRI): %s "
             "(%zu bytes, %u+%u MCUs of %u)",
             gaps.size(), synthetic_dri ? "synthetic" : "native", path,
             output.size(), total_recovered_mcus, post_gap_total,
             cfg.total_mcus);
    return true;
}

bool write_recovered(const RecoveryContext &ctx, const Seed &seed,
                             const ChainResult &chain, int file_num)
{
    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;

    const char *basename = seed.filename.c_str();
    const char *slash = strrchr(basename, '/');
    if (slash) basename = slash + 1;

    char path[1024];
    if (basename[0] && seed.source != SEED_SIGNATURE_SCAN)
        snprintf(path, sizeof(path), "%s/files/%04d_%s", ctx.output_dir.c_str(), file_num, basename);
    else
        snprintf(path, sizeof(path), "%s/files/%04d_cluster_%u.jpg", ctx.output_dir.c_str(), file_num, seed.start_cluster);

    FILE *f = fopen(path, "wb");
    if (!f) { log_error("cannot write %s", path); return false; }

    size_t total = 0;

    if (chain.grafted && !chain.graft_header.empty()) {
        /* Write template header instead of corrupt original */
        fwrite(chain.graft_header.data(), 1, chain.graft_header.size(), f);
        total += chain.graft_header.size();

        /* Write entropy data from first cluster, starting at graft offset */
        const uint8_t *first = ctx.disk.cluster_ptr(chain.clusters[0]);
        if (first && chain.entropy_offset < bpc) {
            size_t entropy_len = bpc - chain.entropy_offset;
            fwrite(first + chain.entropy_offset, 1, entropy_len, f);
            total += entropy_len;
        }

        /* Write remaining clusters normally */
        for (size_t i = 1; i < chain.clusters.size(); i++) {
            const uint8_t *data = ctx.disk.cluster_ptr(chain.clusters[i]);
            if (!data) break;
            size_t write_len = bpc;

            if (i == chain.clusters.size() - 1) {
                for (size_t j = 1; j < write_len; j++) {
                    if (data[j - 1] == 0xFF && data[j] == 0xD9) {
                        write_len = j + 1;
                        break;
                    }
                }
            }
            fwrite(data, 1, write_len, f);
            total += write_len;
        }
    } else {
        /* Normal write: copy clusters as-is.
         * For mid-cluster SOI: skip bytes before the SOI in the first cluster. */
        for (size_t i = 0; i < chain.clusters.size(); i++) {
            const uint8_t *data = ctx.disk.cluster_ptr(chain.clusters[i]);
            if (!data) break;
            size_t write_off = (i == 0) ? seed.soi_offset : 0;
            size_t write_len = bpc - write_off;

            if (i == chain.clusters.size() - 1) {
                if (seed.expected_size > 0 && seed.expected_size > total) {
                    size_t remaining = seed.expected_size - total;
                    if (remaining < write_len) write_len = remaining;
                }
                for (size_t j = 1; j < write_len; j++) {
                    if (data[write_off + j - 1] == 0xFF && data[write_off + j] == 0xD9) {
                        write_len = j + 1;
                        break;
                    }
                }
            }
            fwrite(data + write_off, 1, write_len, f);
            total += write_len;
        }
    }
    fclose(f);

    if (chain.thumb_confidence >= 0.0f) {
        log_info(ctx, "recovered: %s (%zu bytes, %zu clusters, %u MCUs, score=%.2f, thumb=%.2f%s)",
                 path, total, chain.clusters.size(), chain.mcus_recovered,
                 chain.score, chain.thumb_confidence, chain.complete ? ", complete" : "");
    } else {
        log_info(ctx, "recovered: %s (%zu bytes, %zu clusters, %u MCUs, score=%.2f%s)",
                 path, total, chain.clusters.size(), chain.mcus_recovered,
                 chain.score, chain.complete ? ", complete" : "");
    }
    return true;
}

bool write_recovered_variant(const RecoveryContext &ctx, const Seed &seed,
                              const ChainVariant &variant, int file_num,
                              int variant_idx)
{
    if (variant_idx == 0) {
        /* Primary variant: write with normal name */
        return write_recovered(ctx, seed, variant.chain, file_num);
    }

    /* Alternative variant: modify the seed name to include the tag */
    /* We create a temporary modified seed isn't needed - just need a different filename.
     * Construct path directly. */
    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;

    const char *basename = seed.filename.c_str();
    const char *slash = strrchr(basename, '/');
    if (slash) basename = slash + 1;

    /* Insert tag before extension: DSC00123.JPG -> DSC00123_seq.JPG */
    std::string base_str(basename);
    std::string tag = variant.tag;
    size_t dot = base_str.rfind('.');
    std::string new_name;
    if (dot != std::string::npos && basename[0] && seed.source != SEED_SIGNATURE_SCAN)
        new_name = base_str.substr(0, dot) + "_" + tag + base_str.substr(dot);
    else if (basename[0] && seed.source != SEED_SIGNATURE_SCAN)
        new_name = base_str + "_" + tag;
    else
        new_name = "cluster_" + std::to_string(seed.start_cluster) + "_" + tag + ".jpg";

    char path[1024];
    snprintf(path, sizeof(path), "%s/files/%04d_%s",
             ctx.output_dir.c_str(), file_num, new_name.c_str());

    FILE *f = fopen(path, "wb");
    if (!f) { log_error("cannot write %s", path); return false; }

    auto &chain = variant.chain;
    size_t total = 0;

    /* Same write logic as write_recovered - simplified (no graft for alt variants) */
    for (size_t i = 0; i < chain.clusters.size(); i++) {
        const uint8_t *data = ctx.disk.cluster_ptr(chain.clusters[i]);
        if (!data) break;
        size_t write_off = (i == 0) ? seed.soi_offset : 0;
        size_t write_len = bpc - write_off;

        if (i == chain.clusters.size() - 1) {
            if (seed.expected_size > 0 && seed.expected_size > total) {
                size_t remaining = seed.expected_size - total;
                if (remaining < write_len) write_len = remaining;
            }
            for (size_t j = 1; j < write_len; j++) {
                if (data[write_off + j - 1] == 0xFF && data[write_off + j] == 0xD9) {
                    write_len = j + 1;
                    break;
                }
            }
        }
        fwrite(data + write_off, 1, write_len, f);
        total += write_len;
    }
    fclose(f);

    log_info(ctx, "recovered variant [%s]: %s (%zu bytes, %zu clusters, conf=%.2f)",
             variant.tag.c_str(), path, total, chain.clusters.size(),
             variant.confidence);
    return true;
}
