/*
 * metadata.cpp - Pass 3: FAT32 filesystem metadata corruption.
 *
 * Three sub-passes:
 * 1. FAT chain breaks (truncation, cross-linking, invalid pointers)
 * 2. FAT1/FAT2 desync (modify FAT1 entries, leave FAT2 intact)
 * 3. Directory entry corruption (LFN->CJK, start cluster, file size, deletion)
 */
#include "corrsim.h"
#include <cstring>
#include <set>
#include <algorithm>

/* ---- Sub-pass 1: FAT chain breaks ---- */

bool corrupt_fat_chains(SimContext &ctx, uint8_t *data, size_t /*size*/)
{
    auto &cfg = ctx.cfg.metadata;
    auto &geo = ctx.truth.geo;

    if (cfg.fat_chain_break_frac <= 0) return true;

    // Find all allocated FAT entries (value >= 2 and not bad/free)
    std::vector<uint32_t> allocated;
    uint32_t max_clust = geo.total_clusters + 2;
    for (uint32_t c = 2; c < max_clust; c++) {
        uint32_t val = fat32_read_entry(data, geo, c);
        if (val >= 2 || val >= FAT32_EOF_MIN)
            allocated.push_back(c);
    }

    if (allocated.empty()) return true;

    int corrupt_count = std::max(1, (int)(allocated.size() * cfg.fat_chain_break_frac));
    rng_shuffle_u32(ctx, allocated);

    log_info(ctx, "FAT chain corruption: %d / %zu allocated entries",
             corrupt_count, allocated.size());

    for (int i = 0; i < corrupt_count && i < (int)allocated.size(); i++) {
        uint32_t cluster = allocated[i];
        uint32_t old_val = fat32_read_entry(data, geo, cluster);
        uint32_t new_val;

        int action = rng_int(ctx, 0, 3);
        const char *action_name;

        switch (action) {
        case 0: // Set to EOF (truncates chain)
            new_val = 0x0FFFFFFF;
            action_name = "truncate_eof";
            break;
        case 1: // Cross-link: point to a random allocated cluster
            new_val = allocated[rng_int(ctx, 0, (int)allocated.size() - 1)];
            action_name = "cross_link";
            break;
        case 2: // Invalid pointer (beyond data region)
            new_val = max_clust + rng_int(ctx, 100, 10000);
            action_name = "invalid_ptr";
            break;
        case 3: // Set to free (lost chain)
            new_val = 0;
            action_name = "lost_chain";
            break;
        default:
            new_val = 0x0FFFFFFF;
            action_name = "truncate_eof";
        }

        // Write to both FAT copies (realistic: both get corrupted together
        // during a failed write)
        fat32_write_entry(data, geo, cluster, new_val, 0);
        fat32_write_entry(data, geo, cluster, new_val, 1);

        ctx.truth.fat_entries_corrupted++;

        char detail[128];
        snprintf(detail, sizeof(detail), "cluster %u: 0x%08X -> 0x%08X (%s)",
                 cluster, old_val, new_val, action_name);
        ctx.truth.mutations.push_back({
            "fat_chain_break",
            geo.fat1_offset + (uint64_t)cluster * 4,
            4, detail
        });

        log_detail(ctx, "FAT break: %s", detail);
    }

    return true;
}

/* ---- Sub-pass 2: FAT1/FAT2 desync ---- */

static void corrupt_fat_desync(SimContext &ctx, uint8_t *data, size_t /*size*/)
{
    auto &cfg = ctx.cfg.metadata;
    auto &geo = ctx.truth.geo;

    if (cfg.fat_desync_entries <= 0) return;

    // Pick a contiguous range in FAT1 to modify (leave FAT2 pristine)
    uint32_t max_start = geo.total_clusters + 2 - cfg.fat_desync_entries;
    if (max_start < 2) return;

    uint32_t start = rng_int(ctx, 2, (int)max_start);

    log_info(ctx, "FAT desync: modifying FAT1 entries %u-%u (FAT2 untouched)",
             start, start + cfg.fat_desync_entries - 1);

    for (int i = 0; i < cfg.fat_desync_entries; i++) {
        uint32_t cluster = start + i;
        uint32_t old_val = fat32_read_entry(data, geo, cluster);
        uint32_t new_val;

        // Simulate a partial FAT update (newer state in FAT1)
        int action = rng_int(ctx, 0, 2);
        switch (action) {
        case 0: new_val = 0; break;  // freed
        case 1: new_val = 0x0FFFFFFF; break;  // EOF
        case 2: // small offset change
            new_val = old_val + rng_int(ctx, -5, 5);
            if (new_val < 2) new_val = 0x0FFFFFFF;
            break;
        default: new_val = 0;
        }

        // Only modify FAT1
        fat32_write_entry(data, geo, cluster, new_val, 0);

        ctx.truth.fat_entries_corrupted++;

        char detail[128];
        snprintf(detail, sizeof(detail),
                 "FAT1 entry %u: 0x%08X -> 0x%08X (FAT2 still 0x%08X)",
                 cluster, old_val, new_val, old_val);
        ctx.truth.mutations.push_back({
            "fat_desync",
            geo.fat1_offset + (uint64_t)cluster * 4,
            4, detail
        });

        log_detail(ctx, "FAT desync: %s", detail);
    }
}

/* ---- Sub-pass 3: Directory entry corruption ---- */

static void corrupt_dir_entries(SimContext &ctx, uint8_t *data, size_t size)
{
    auto &cfg = ctx.cfg.metadata;
    auto &geo = ctx.truth.geo;

    if (cfg.dir_entry_corrupt_frac <= 0) return;

    // Collect all directory entry offsets by walking the directory tree
    struct DirEntryLoc {
        uint64_t offset;
        bool is_lfn;
        uint8_t attr;
    };
    std::vector<DirEntryLoc> entries;

    // Walk from root cluster
    std::vector<uint32_t> dir_clusters_to_scan;
    dir_clusters_to_scan.push_back(geo.root_cluster);
    std::set<uint32_t> visited_clusters;

    while (!dir_clusters_to_scan.empty()) {
        uint32_t dir_start = dir_clusters_to_scan.back();
        dir_clusters_to_scan.pop_back();

        if (visited_clusters.count(dir_start)) continue;
        visited_clusters.insert(dir_start);

        // Follow chain for this directory
        uint32_t c = dir_start;
        uint32_t max_c = geo.total_clusters + 2;
        std::set<uint32_t> chain_visited;

        while (c >= 2 && c < max_c && !chain_visited.count(c)) {
            chain_visited.insert(c);
            uint64_t off = fat32_cluster_offset(geo, c);
            if (off + geo.bytes_per_cluster > size) break;

            for (uint32_t i = 0; i < geo.bytes_per_cluster; i += 32) {
                uint8_t *ent = data + off + i;
                if (ent[0] == 0x00) goto done_dir;
                if (ent[0] == 0xE5) continue;

                if (ent[11] == 0x0F) {
                    // LFN entry
                    entries.push_back({off + i, true, 0x0F});
                } else {
                    uint8_t attr = ent[11];
                    // Skip . and .. entries
                    if (ent[0] == '.' && (ent[1] == ' ' || ent[1] == '.'))
                        continue;

                    entries.push_back({off + i, false, attr});

                    // If it's a directory, queue it for scanning
                    if (attr & 0x10) {
                        uint32_t sub_start =
                            ((uint32_t)read16(ent + 20) << 16) | read16(ent + 26);
                        if (sub_start >= 2 && sub_start < max_c)
                            dir_clusters_to_scan.push_back(sub_start);
                    }
                }
            }

            uint32_t next = fat32_read_entry(data, geo, c);
            if (next >= FAT32_EOF_MIN) break;
            c = next;
        }
        done_dir:;
    }

    if (entries.empty()) return;

    int corrupt_count = std::max(1, (int)(entries.size() * cfg.dir_entry_corrupt_frac));

    // Shuffle and pick entries to corrupt
    std::vector<uint32_t> indices;
    for (uint32_t i = 0; i < (uint32_t)entries.size(); i++)
        indices.push_back(i);
    rng_shuffle_u32(ctx, indices);

    log_info(ctx, "Dir entry corruption: %d / %zu entries",
             corrupt_count, entries.size());

    int deleted_marks_remaining = cfg.deleted_marks;
    bool eod_placed = false;

    for (int i = 0; i < corrupt_count && i < (int)indices.size(); i++) {
        auto &loc = entries[indices[i]];
        uint8_t *ent = data + loc.offset;
        const char *action_name;

        if (loc.is_lfn && cfg.lfn_cjk_corruption) {
            // Corrupt LFN name bytes with random data
            // LFN name bytes at: 0x01-0x0A (5 chars), 0x0E-0x19 (6 chars), 0x1C-0x1F (2 chars)
            static const int lfn_offsets[] = {
                1, 3, 5, 7, 9,          // chars 1-5
                14, 16, 18, 20, 22, 24, // chars 6-11
                28, 30                   // chars 12-13
            };
            int num_chars_to_corrupt = rng_int(ctx, 2, 8);
            for (int j = 0; j < num_chars_to_corrupt && j < 13; j++) {
                int off = lfn_offsets[j];
                ent[off]     = rng_int(ctx, 0, 255);
                ent[off + 1] = rng_int(ctx, 0, 255);
            }
            action_name = "lfn_name_corrupt";

        } else if (!loc.is_lfn) {
            // SFN entry corruption
            int action = rng_int(ctx, 0, 5);

            switch (action) {
            case 0: // Corrupt start cluster
                if (cfg.cross_link) {
                    uint32_t fake = rng_int(ctx, 2, (int)(geo.total_clusters + 1));
                    write16(ent + 20, (uint16_t)(fake >> 16));
                    write16(ent + 26, (uint16_t)(fake & 0xFFFF));
                    action_name = "start_cluster_corrupt";
                } else {
                    action_name = "skipped";
                }
                break;

            case 1: // Corrupt file size
            {
                uint32_t old_size = read32(ent + 28);
                uint32_t new_size = old_size ^ (1 << rng_int(ctx, 0, 31));
                write32(ent + 28, new_size);
                action_name = "file_size_corrupt";
                break;
            }

            case 2: // Mark as deleted (0xE5)
                if (deleted_marks_remaining > 0) {
                    ent[0] = 0xE5;
                    deleted_marks_remaining--;
                    action_name = "delete_mark";
                } else {
                    action_name = "skipped";
                }
                break;

            case 3: // Premature end-of-directory (0x00)
                if (cfg.premature_eod && !eod_placed) {
                    ent[0] = 0x00;
                    eod_placed = true;
                    action_name = "premature_eod";
                } else {
                    action_name = "skipped";
                }
                break;

            case 4: // Attribute corruption: set to 0x0F (LFN signature)
                ent[11] = 0x0F;
                action_name = "attr_to_lfn";
                break;

            case 5: // Corrupt filename bytes
                for (int j = 0; j < 8; j++)
                    ent[j] = rng_int(ctx, 0, 255);
                action_name = "sfn_name_corrupt";
                break;

            default:
                action_name = "unknown";
            }
        } else {
            action_name = "skipped";
        }

        if (strcmp(action_name, "skipped") != 0) {
            ctx.truth.dir_entries_corrupted++;

            char detail[128];
            snprintf(detail, sizeof(detail), "dir entry at offset %lu: %s",
                     (unsigned long)loc.offset, action_name);
            ctx.truth.mutations.push_back({
                "dir_entry_corrupt", loc.offset, 32, detail
            });

            log_detail(ctx, "Dir corrupt: %s", detail);
        }
    }
}

bool corrupt_metadata(SimContext &ctx, uint8_t *data, size_t size)
{
    log_progress(ctx, "  Pass 3: Metadata corruption");

    // Need valid geometry
    if (ctx.truth.geo.total_clusters == 0) {
        Fat32Geo geo;
        if (!fat32_parse_geo(data, size, geo)) {
            log_error("Cannot parse FAT32 for metadata corruption");
            return false;
        }
        ctx.truth.geo = geo;
    }

    corrupt_fat_chains(ctx, data, size);
    corrupt_fat_desync(ctx, data, size);
    corrupt_dir_entries(ctx, data, size);

    log_progress(ctx, "  Metadata: %u FAT entries, %u dir entries corrupted",
                 ctx.truth.fat_entries_corrupted,
                 ctx.truth.dir_entries_corrupted);

    return true;
}
