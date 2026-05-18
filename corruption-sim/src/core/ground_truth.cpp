/*
 * ground_truth.cpp - Stage 4: Parse FAT32 image natively to record
 * ground truth before corruption.
 */
#include "corrsim.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <sstream>

/* ---- FAT32 helpers (shared with metadata.cpp) ---- */

bool fat32_parse_geo(const uint8_t *data, size_t size, Fat32Geo &geo)
{
    if (size < 512) return false;

    // Check boot signature
    if (data[510] != 0x55 || data[511] != 0xAA)
        return false;

    geo.bytes_per_sector    = read16(data + 11);
    geo.sectors_per_cluster = data[13];
    geo.reserved_sectors    = read16(data + 14);
    geo.num_fats            = data[16];
    geo.sectors_per_fat     = read32(data + 36);
    geo.root_cluster        = read32(data + 44);
    geo.total_sectors       = read32(data + 32);
    if (geo.total_sectors == 0)
        geo.total_sectors   = read16(data + 19);

    geo.bytes_per_cluster = geo.bytes_per_sector * geo.sectors_per_cluster;

    uint32_t data_start_sector = geo.reserved_sectors +
                                 geo.num_fats * geo.sectors_per_fat;
    uint32_t data_sectors = geo.total_sectors - data_start_sector;
    geo.total_clusters = data_sectors / geo.sectors_per_cluster;

    geo.fat1_offset = (uint64_t)geo.reserved_sectors * geo.bytes_per_sector;
    geo.fat2_offset = geo.fat1_offset +
                      (uint64_t)geo.sectors_per_fat * geo.bytes_per_sector;
    geo.data_offset = (uint64_t)data_start_sector * geo.bytes_per_sector;

    return geo.bytes_per_sector >= 512 && geo.sectors_per_cluster > 0 &&
           geo.total_clusters > 0;
}

uint32_t fat32_read_entry(const uint8_t *data, const Fat32Geo &geo, uint32_t cluster)
{
    uint64_t off = geo.fat1_offset + (uint64_t)cluster * 4;
    return read32(data + off) & FAT32_ENTRY_MASK;
}

void fat32_write_entry(uint8_t *data, const Fat32Geo &geo, uint32_t cluster,
                       uint32_t value, int fat_num)
{
    uint64_t base = (fat_num == 1) ? geo.fat2_offset : geo.fat1_offset;
    uint64_t off = base + (uint64_t)cluster * 4;
    // Preserve top 4 bits
    uint32_t existing = read32(data + off);
    uint32_t new_val = (existing & 0xF0000000) | (value & FAT32_ENTRY_MASK);
    write32(data + off, new_val);
}

uint64_t fat32_cluster_offset(const Fat32Geo &geo, uint32_t cluster)
{
    return geo.data_offset + (uint64_t)(cluster - 2) * geo.bytes_per_cluster;
}

/* Follow a cluster chain through the FAT */
static std::vector<uint32_t> follow_chain(const uint8_t *data,
                                          const Fat32Geo &geo,
                                          uint32_t start)
{
    std::vector<uint32_t> chain;
    uint32_t c = start;
    uint32_t max_clusters = geo.total_clusters + 2;

    while (c >= 2 && c < max_clusters && chain.size() < 100000) {
        chain.push_back(c);
        uint32_t next = fat32_read_entry(data, geo, c);
        if (next >= FAT32_EOF_MIN) break;  // end of chain
        if (next < 2 || next >= max_clusters) break;  // corrupt
        c = next;
    }
    return chain;
}

/* Compute SHA-256 of file data by extracting cluster contents */
static std::string compute_sha256(const uint8_t *data, const Fat32Geo &geo,
                                  const std::vector<uint32_t> &clusters,
                                  uint32_t file_size)
{
    // Write cluster data to a temp file, then sha256sum it
    char tmppath[] = "/tmp/corrsim_sha_XXXXXX";
    int fd = mkstemp(tmppath);
    if (fd < 0) return "";

    uint32_t remaining = file_size;
    for (uint32_t c : clusters) {
        uint64_t off = fat32_cluster_offset(geo, c);
        uint32_t chunk = std::min(remaining, geo.bytes_per_cluster);
        if (write(fd, data + off, chunk) != (ssize_t)chunk) {
            close(fd);
            unlink(tmppath);
            return "";
        }
        remaining -= chunk;
        if (remaining == 0) break;
    }
    close(fd);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "sha256sum '%s'", tmppath);
    auto r = run_cmd(cmd);
    unlink(tmppath);

    if (r.exit_code != 0 || r.out.size() < 64)
        return "";
    return r.out.substr(0, 64);
}

/* Parse a single 32-byte directory entry */
struct DirEntry {
    std::string short_name;
    uint32_t start_cluster = 0;
    uint32_t file_size = 0;
    uint8_t  attr = 0;
    bool     is_lfn = false;
    bool     is_deleted = false;
    bool     is_end = false;
};

static DirEntry parse_dir_entry(const uint8_t *ent)
{
    DirEntry d;
    if (ent[0] == 0x00) { d.is_end = true; return d; }
    if (ent[0] == 0xE5) { d.is_deleted = true; }
    if (ent[11] == 0x0F) { d.is_lfn = true; return d; }

    d.attr = ent[11];

    // Short name (first byte is 0xE5 for deleted entries, replace with '_')
    char name[13];
    int ni = 0;
    for (int i = 0; i < 8; i++) {
        if (ent[i] == ' ') break;
        char c = ent[i];
        if (i == 0 && d.is_deleted) c = '_';
        name[ni++] = c;
    }
    if (ent[8] != ' ') {
        name[ni++] = '.';
        for (int i = 8; i < 11; i++) {
            if (ent[i] == ' ') break;
            name[ni++] = ent[i];
        }
    }
    name[ni] = '\0';
    d.short_name = name;

    d.start_cluster = ((uint32_t)read16(ent + 20) << 16) | read16(ent + 26);
    d.file_size = read32(ent + 28);
    return d;
}

/* Extract a UTF-16LE character from LFN entry and append as ASCII/UTF-8 */
static void lfn_extract_chars(const uint8_t *ent, std::string &name)
{
    // LFN name chars at: 0x01-0x0A (5 chars), 0x0E-0x19 (6 chars), 0x1C-0x1F (2 chars)
    static const int offsets[] = {1,3,5,7,9, 14,16,18,20,22,24, 28,30};
    for (int off : offsets) {
        uint16_t ch = read16(ent + off);
        if (ch == 0x0000 || ch == 0xFFFF) return;
        // Simple ASCII passthrough; non-ASCII gets '?'
        if (ch < 128)
            name += (char)ch;
        else
            name += '?';
    }
}

/* Recursively walk directory tree */
static void walk_directory(const uint8_t *data, size_t data_size,
                           const Fat32Geo &geo,
                           uint32_t dir_cluster,
                           const std::string &path_prefix,
                           SimContext &ctx)
{
    auto chain = follow_chain(data, geo, dir_cluster);
    if (chain.empty()) return;

    // Collect all entries across clusters for LFN reconstruction
    struct RawEntry {
        const uint8_t *ptr;
    };
    std::vector<RawEntry> all_entries;

    for (uint32_t clust : chain) {
        uint64_t off = fat32_cluster_offset(geo, clust);
        if (off + geo.bytes_per_cluster > data_size) return;
        for (uint32_t i = 0; i < geo.bytes_per_cluster; i += 32) {
            const uint8_t *ent = data + off + i;
            if (ent[0] == 0x00) goto entries_done;
            all_entries.push_back({ent});
        }
    }
    entries_done:

    // Helper: reconstruct LFN from collected parts
    // For deleted entries, byte 0 is 0xE5, so sequence numbers are lost.
    // LFN entries appear in reverse order before SFN, so we reverse
    // and extract chars. For non-deleted entries we sort by sequence.
    auto build_lfn = [](const std::vector<const uint8_t *> &parts,
                        bool deleted) -> std::string {
        if (parts.empty()) return "";
        std::string name;
        if (deleted) {
            // Reverse order (LFN parts are stored last-to-first before SFN)
            for (int i = (int)parts.size() - 1; i >= 0; i--)
                lfn_extract_chars(parts[i], name);
        } else {
            std::vector<std::pair<int, const uint8_t *>> ordered;
            for (auto *lp : parts) {
                int seq = lp[0] & 0x3F;
                ordered.push_back({seq, lp});
            }
            std::sort(ordered.begin(), ordered.end());
            for (auto &[seq, lp] : ordered)
                lfn_extract_chars(lp, name);
        }
        return name;
    };

    // Process entries, collecting LFN parts before each SFN
    std::vector<const uint8_t *> lfn_parts;

    for (auto &re : all_entries) {
        const uint8_t *ent = re.ptr;
        DirEntry d = parse_dir_entry(ent);

        if (d.is_lfn) {
            if (d.is_deleted) {
                // Deleted LFN entry -- still collect for deleted file reconstruction
                lfn_parts.push_back(ent);
            } else {
                lfn_parts.push_back(ent);
            }
            continue;
        }

        if (d.is_deleted) {
            // Deleted SFN entry: try to reconstruct for deleted_files
            if (!(d.attr & 0x10) && !(d.attr & 0x08) && d.start_cluster >= 2) {
                std::string display_name = build_lfn(lfn_parts, true);
                if (display_name.empty())
                    display_name = d.short_name;

                std::string full_path = path_prefix.empty()
                    ? display_name : path_prefix + "/" + display_name;

                // Deleted file's clusters may have been reused, but we
                // can still record the start cluster and file size
                auto clusters = follow_chain(data, geo, d.start_cluster);

                FileRecord rec;
                rec.image_path = full_path;
                rec.clusters = clusters;
                rec.file_size = d.file_size;
                rec.data_offset = clusters.empty() ? 0
                    : fat32_cluster_offset(geo, clusters[0]);
                rec.was_deleted = true;

                // SHA-256 might be wrong if clusters were reallocated,
                // but record it for reference
                if (!clusters.empty())
                    rec.sha256 = compute_sha256(data, geo, clusters, d.file_size);

                ctx.truth.deleted_files.push_back(std::move(rec));
                log_debug(ctx, "Deleted file: /%s  %u bytes  start_cluster=%u",
                          full_path.c_str(), d.file_size, d.start_cluster);
            }
            lfn_parts.clear();
            continue;
        }

        // Active SFN entry
        if (d.short_name == "." || d.short_name == "..") {
            lfn_parts.clear(); continue;
        }
        if ((d.attr & 0x08) && !(d.attr & 0x10)) {
            lfn_parts.clear(); continue;
        }

        std::string display_name = build_lfn(lfn_parts, false);
        lfn_parts.clear();

        if (display_name.empty())
            display_name = d.short_name;

        std::string full_path = path_prefix.empty()
            ? display_name
            : path_prefix + "/" + display_name;

        if (d.attr & 0x10) {
            if (d.start_cluster >= 2) {
                walk_directory(data, data_size, geo, d.start_cluster,
                              full_path, ctx);
            }
        } else {
            auto clusters = follow_chain(data, geo, d.start_cluster);
            std::string sha = compute_sha256(data, geo, clusters, d.file_size);

            FileRecord rec;
            rec.image_path = full_path;
            rec.clusters = clusters;
            rec.file_size = d.file_size;
            rec.data_offset = clusters.empty() ? 0
                : fat32_cluster_offset(geo, clusters[0]);
            rec.sha256 = sha;

            ctx.truth.files.push_back(std::move(rec));
            log_debug(ctx, "Ground truth: /%s  %u bytes  %zu clusters  sha=%s",
                      full_path.c_str(), d.file_size, clusters.size(),
                      sha.c_str());
        }
    }
}

bool ground_truth_record(SimContext &ctx)
{
    log_progress(ctx, "Stage 4: Recording ground truth");

    int fd = open(ctx.cfg.image_path.c_str(), O_RDONLY);
    if (fd < 0) {
        log_error("cannot open image: %s", ctx.cfg.image_path.c_str());
        return false;
    }

    struct stat st;
    fstat(fd, &st);
    size_t size = st.st_size;

    const uint8_t *data = (const uint8_t *)mmap(nullptr, size, PROT_READ,
                                                 MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        log_error("mmap failed");
        close(fd);
        return false;
    }

    Fat32Geo &geo = ctx.truth.geo;
    if (!fat32_parse_geo(data, size, geo)) {
        log_error("FAT32 parse failed - invalid filesystem");
        munmap((void *)data, size);
        close(fd);
        return false;
    }

    log_info(ctx, "FAT32 geometry: %u bytes/sector, %u sectors/cluster, "
             "%u total clusters",
             geo.bytes_per_sector, geo.sectors_per_cluster, geo.total_clusters);
    log_info(ctx, "FAT1 at offset %lu, data at offset %lu",
             (unsigned long)geo.fat1_offset, (unsigned long)geo.data_offset);

    // Walk directory tree from root
    walk_directory(data, size, geo, geo.root_cluster, "", ctx);

    // Cross-reference with placement records to set original_path
    // Build a lookup from image_path -> source_path
    std::unordered_map<std::string, std::string> path_to_source;
    for (auto &pf : ctx.placed_files)
        path_to_source[pf.image_path] = pf.source_path;

    for (auto &fr : ctx.truth.files) {
        auto it = path_to_source.find(fr.image_path);
        if (it != path_to_source.end()) {
            fr.original_path = it->second;
            // Compute SHA-256 of the clean source file for reference
            char cmd[512];
            snprintf(cmd, sizeof(cmd), "sha256sum '%s'", it->second.c_str());
            auto r = run_cmd(cmd);
            if (r.exit_code == 0 && r.out.size() >= 64)
                fr.sha256_source = r.out.substr(0, 64);
        }
    }
    for (auto &fr : ctx.truth.deleted_files) {
        auto it = path_to_source.find(fr.image_path);
        if (it != path_to_source.end())
            fr.original_path = it->second;
    }

    // Mark moved files from sim_ops
    for (auto &op : ctx.sim_ops) {
        if (op.type == "move") {
            // Find the file at its new destination and mark it
            for (auto &fr : ctx.truth.files) {
                if (fr.image_path == op.dest) {
                    fr.was_moved = true;
                    fr.moved_from = op.path;
                    break;
                }
            }
        }
    }

    log_progress(ctx, "  Recorded %zu files, %zu deleted with cluster chains and SHA-256",
                 ctx.truth.files.size(), ctx.truth.deleted_files.size());

    munmap((void *)data, size);
    close(fd);
    return true;
}

/* ---- Manifest writer ---- */

bool manifest_write(const SimContext &ctx)
{
    std::string path = ctx.cfg.manifest_path;
    if (path.empty())
        path = ctx.cfg.image_path + ".manifest.json";

    FILE *f = fopen(path.c_str(), "w");
    if (!f) {
        log_error("cannot open manifest: %s", path.c_str());
        return false;
    }

    JsonWriter jw(f);
    jw.begin_object();

    jw.key("version"); jw.value_string(CORRSIM_VERSION);
    jw.key("rng_seed"); jw.value_uint(ctx.cfg.rng_seed);
    if (!ctx.cfg.profile.empty()) {
        jw.key("profile"); jw.value_string(ctx.cfg.profile);
    }
    jw.key("image_path"); jw.value_string(ctx.cfg.image_path);

    // Geometry
    jw.key("geometry");
    jw.begin_object();
    jw.key("bytes_per_sector"); jw.value_uint(ctx.truth.geo.bytes_per_sector);
    jw.key("sectors_per_cluster"); jw.value_uint(ctx.truth.geo.sectors_per_cluster);
    jw.key("bytes_per_cluster"); jw.value_uint(ctx.truth.geo.bytes_per_cluster);
    jw.key("reserved_sectors"); jw.value_uint(ctx.truth.geo.reserved_sectors);
    jw.key("total_clusters"); jw.value_uint(ctx.truth.geo.total_clusters);
    jw.key("fat1_offset"); jw.value_uint(ctx.truth.geo.fat1_offset);
    jw.key("fat2_offset"); jw.value_uint(ctx.truth.geo.fat2_offset);
    jw.key("data_offset"); jw.value_uint(ctx.truth.geo.data_offset);
    jw.end_object();

    // Config used
    jw.key("config");
    jw.begin_object();
    jw.key("bitflip");
    jw.begin_object();
    jw.key("ber"); jw.value_double(ctx.cfg.bitflip.ber, 2);
    jw.key("bias_0to1"); jw.value_double(ctx.cfg.bitflip.bias_0to1);
    jw.key("degraded_frac"); jw.value_double(ctx.cfg.bitflip.degraded_frac);
    jw.key("erase_block_size"); jw.value_uint(ctx.cfg.bitflip.erase_block_size);
    jw.end_object();
    jw.key("ftl");
    jw.begin_object();
    jw.key("swap_frac"); jw.value_double(ctx.cfg.ftl.swap_frac);
    jw.key("zero_frac"); jw.value_double(ctx.cfg.ftl.zero_frac);
    jw.key("wrong_data_frac"); jw.value_double(ctx.cfg.ftl.wrong_data_frac);
    jw.key("block_size"); jw.value_uint(ctx.cfg.ftl.block_size);
    jw.end_object();
    jw.key("metadata");
    jw.begin_object();
    jw.key("fat_chain_break_frac"); jw.value_double(ctx.cfg.metadata.fat_chain_break_frac);
    jw.key("dir_entry_corrupt_frac"); jw.value_double(ctx.cfg.metadata.dir_entry_corrupt_frac);
    jw.key("fat_desync_entries"); jw.value_int(ctx.cfg.metadata.fat_desync_entries);
    jw.end_object();
    if (ctx.cfg.pre_corrupt_frac > 0)
        { jw.key("pre_corrupt_frac"); jw.value_double(ctx.cfg.pre_corrupt_frac); }
    if (ctx.cfg.fragment.enabled) {
        jw.key("fragment");
        jw.begin_object();
        jw.key("file_count"); jw.value_int(ctx.cfg.fragment.file_count);
        jw.key("min_size"); jw.value_uint(ctx.cfg.fragment.min_size);
        jw.key("max_size"); jw.value_uint(ctx.cfg.fragment.max_size);
        jw.key("delete_frac"); jw.value_double(ctx.cfg.fragment.delete_frac);
        jw.end_object();
    }
    jw.end_object();

    // Files
    jw.key("files");
    jw.begin_array();
    for (auto &fr : ctx.truth.files) {
        jw.begin_object();
        if (!fr.original_path.empty()) {
            jw.key("original"); jw.value_string(fr.original_path);
        }
        jw.key("image_path"); jw.value_string(fr.image_path);
        jw.key("file_size"); jw.value_uint(fr.file_size);
        jw.key("data_offset"); jw.value_uint(fr.data_offset);
        jw.key("sha256"); jw.value_string(fr.sha256);
        if (!fr.sha256_source.empty())
            { jw.key("sha256_source"); jw.value_string(fr.sha256_source); }
        if (fr.was_moved) {
            jw.key("moved"); jw.value_bool(true);
            jw.key("moved_from"); jw.value_string(fr.moved_from);
        }

        jw.key("clusters");
        jw.begin_array();
        for (uint32_t c : fr.clusters)
            jw.value_uint(c);
        jw.end_array();

        jw.end_object();
    }
    jw.end_array();

    // Deleted files
    if (!ctx.truth.deleted_files.empty()) {
        jw.key("deleted_files");
        jw.begin_array();
        for (auto &fr : ctx.truth.deleted_files) {
            jw.begin_object();
            if (!fr.original_path.empty()) {
                jw.key("original"); jw.value_string(fr.original_path);
            }
            jw.key("image_path"); jw.value_string(fr.image_path);
            jw.key("file_size"); jw.value_uint(fr.file_size);
            if (!fr.sha256.empty()) {
                jw.key("sha256"); jw.value_string(fr.sha256);
            }
            if (!fr.clusters.empty()) {
                jw.key("start_cluster"); jw.value_uint(fr.clusters[0]);
                jw.key("clusters");
                jw.begin_array();
                for (uint32_t c : fr.clusters)
                    jw.value_uint(c);
                jw.end_array();
            }
            jw.end_object();
        }
        jw.end_array();
    }

    // Pre-corruption summary (if applicable)
    if (ctx.truth.pre_bits_flipped > 0 || ctx.truth.pre_blocks_zeroed > 0 ||
        ctx.truth.pre_fat_entries_corrupted > 0) {
        jw.key("pre_corruption_summary");
        jw.begin_object();
        jw.key("bits_flipped"); jw.value_uint(ctx.truth.pre_bits_flipped);
        jw.key("blocks_zeroed"); jw.value_uint(ctx.truth.pre_blocks_zeroed);
        jw.key("blocks_swapped"); jw.value_uint(ctx.truth.pre_blocks_swapped);
        jw.key("blocks_wrong_data"); jw.value_uint(ctx.truth.pre_blocks_wrong_data);
        jw.key("fat_entries_corrupted"); jw.value_uint(ctx.truth.pre_fat_entries_corrupted);
        jw.end_object();
    }

    // Post-corruption summary
    jw.key("corruption_summary");
    jw.begin_object();
    jw.key("total_bits_flipped"); jw.value_uint(ctx.truth.total_bits_flipped);
    jw.key("blocks_zeroed"); jw.value_uint(ctx.truth.blocks_zeroed);
    jw.key("blocks_swapped"); jw.value_uint(ctx.truth.blocks_swapped);
    jw.key("blocks_wrong_data"); jw.value_uint(ctx.truth.blocks_wrong_data);
    jw.key("fat_entries_corrupted"); jw.value_uint(ctx.truth.fat_entries_corrupted);
    jw.key("dir_entries_corrupted"); jw.value_uint(ctx.truth.dir_entries_corrupted);
    jw.end_object();

    // Mutations (optionally full detail)
    if (ctx.cfg.full_manifest && !ctx.truth.mutations.empty()) {
        jw.key("mutations");
        jw.begin_array();
        for (auto &m : ctx.truth.mutations) {
            jw.begin_object();
            jw.key("type"); jw.value_string(m.type);
            jw.key("offset"); jw.value_uint(m.offset);
            jw.key("length"); jw.value_uint(m.length);
            jw.key("detail"); jw.value_string(m.detail);
            jw.end_object();
        }
        jw.end_array();
    }

    jw.end_object();
    fprintf(f, "\n");
    fclose(f);

    log_progress(ctx, "  Manifest written: %s", path.c_str());
    return true;
}
