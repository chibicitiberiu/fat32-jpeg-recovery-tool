/*
 * seed.cpp - Seed list builder
 *
 * Directory tree walk + JPEG SOI signature scan.
 * Deduplicates by start_cluster.
 */
#include "sdrecov.h"
#include <cstring>
#include <algorithm>
#include <unordered_set>

static uint16_t read16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t read32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static std::string sanitize(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c >= 0x20 && c < 0x7F && c != '<' && c != '>' && c != ':' &&
            c != '"' && c != '|' && c != '?' && c != '*')
            out += c;
        else
            out += '_';
    }
    return out;
}

static std::string extract_short_name(const uint8_t *entry)
{
    char name[9] = {}, ext[4] = {};
    memcpy(name, entry, 8);
    memcpy(ext, entry + 8, 3);

    for (int i = 7; i >= 0 && name[i] == ' '; i--) name[i] = 0;
    for (int i = 2; i >= 0 && ext[i] == ' '; i--) ext[i] = 0;

    std::string result;
    if (ext[0])
        result = std::string(name) + "." + ext;
    else
        result = name;

    return sanitize(result);
}

static std::unordered_set<uint32_t> visited_dirs;

static void walk_directory(RecoveryContext &ctx, uint32_t dir_cluster,
                           const std::string &parent_path, int depth)
{
    if (depth > 16) return;
    if (parent_path.size() > 512) return; /* path too long = circular */
    if (visited_dirs.count(dir_cluster)) return; /* already walked this dir */
    visited_dirs.insert(dir_cluster);

    uint32_t cluster = dir_cluster;
    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;

    while (cluster >= 2 && cluster < ctx.disk.geo.total_clusters + 2) {
        const uint8_t *data = ctx.disk.cluster_ptr(cluster);
        if (!data) break;

        for (uint32_t off = 0; off + 32 <= bpc; off += 32) {
            const uint8_t *e = data + off;
            if (e[0] == 0x00) return;     /* end of directory */
            if (e[0] == 0xE5) continue;   /* deleted */

            uint8_t attr = e[11];
            if (attr == 0x0F) continue;   /* LFN */
            if (attr & 0x08) continue;    /* volume label */

            uint32_t start = (uint32_t(read16(e + 20)) << 16) | read16(e + 26);
            uint32_t fsize = read32(e + 28);

            if (start < 2 || start >= ctx.disk.geo.total_clusters + 2) continue;
            if (fsize == 0 && !(attr & 0x10)) continue;

            if (attr & 0x10) {
                /* Directory: recurse (skip . and ..) */
                if (e[0] != '.') {
                    auto dirname = extract_short_name(e);
                    walk_directory(ctx, start, parent_path + dirname + "/", depth + 1);
                }
            } else {
                /* File */
                Seed s;
                s.start_cluster    = start;
                s.expected_size    = fsize;
                s.expected_clusters = (fsize + bpc - 1) / bpc;
                s.source           = SEED_DIR_ENTRY;
                s.confidence       = CONF_HIGH;
                s.jpeg_mode        = JPEG_UNKNOWN;
                s.filename         = parent_path + extract_short_name(e);
                ctx.seeds.push_back(std::move(s));
            }
        }

        /* Follow chain */
        uint32_t next = ctx.fat.merged[cluster];
        if (next < 2 || next >= ctx.disk.geo.total_clusters + 2 || next == cluster)
            break;
        cluster = next;
    }
}

/*
 * Extract EXIF thumbnails for seeds that point to JPEG headers.
 * Stores thumbnail offset/size in the seed for later MAE validation.
 */
static void extract_thumbnails(RecoveryContext &ctx)
{
    uint32_t bpc = ctx.disk.geo.bytes_per_cluster;
    int found = 0;

    for (auto &seed : ctx.seeds) {
        if (seed.source == SEED_SIGNATURE_SCAN) continue;

        const uint8_t *data = ctx.disk.cluster_ptr(seed.start_cluster);
        if (!data) continue;
        if (data[0] != 0xFF || data[1] != 0xD8) continue;

        /* EXIF APP1 segments can be up to 64KB, spanning multiple clusters.
         * Concatenate header clusters (via FAT chain or sequential) to cover
         * the full EXIF segment where the thumbnail lives. */
        std::vector<uint8_t> hdr_buf(data, data + bpc);
        uint32_t cl = seed.start_cluster;
        for (int i = 1; i < 16 && hdr_buf.size() < 65536; i++) {
            uint32_t next = 0;
            if (cl < ctx.fat.count() && ctx.fat.status[cl] == FAT_VALID)
                next = ctx.fat.merged[cl];
            if (next < 2 || next > ctx.disk.geo.total_clusters + 1)
                next = seed.start_cluster + i; /* fallback: sequential */
            const uint8_t *nd = ctx.disk.cluster_ptr(next);
            if (!nd) break;
            hdr_buf.insert(hdr_buf.end(), nd, nd + bpc);
            cl = next;
        }

        uint32_t offset = 0, size = 0;
        if (jpeg_extract_thumbnail(hdr_buf.data(), hdr_buf.size(), offset, size)) {
            seed.has_thumbnail = true;
            seed.thumbnail_offset = offset;
            seed.thumbnail_size = size;
            found++;
        }
    }

    log_debug(ctx, "Thumbnails: %d found in %d dir-entry seeds", found, (int)ctx.seeds.size());
}

static void scan_signatures(RecoveryContext &ctx)
{
    /* Build set of existing start clusters for fast lookup */
    std::unordered_set<uint32_t> existing;
    for (auto &s : ctx.seeds)
        existing.insert(s.start_cluster);

    uint32_t n = ctx.disk.geo.total_clusters;
    for (uint32_t i = 0; i < n; i++) {
        if (!(ctx.cluster_map[i].flags & CF_HAS_SOI))
            continue;

        uint32_t cluster = i + 2;
        const uint8_t *data = ctx.disk.cluster_ptr(cluster);
        if (!data) continue;

        /* Find FFD8FF - first at offset 0, then scan first 512 bytes */
        int soi_off = -1;
        if (data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
            soi_off = 0;
        } else if (ctx.features.mid_cluster_soi) {
            uint32_t bpc = ctx.disk.geo.bytes_per_cluster;
            size_t scan_limit = std::min((size_t)512, (size_t)bpc - 2);
            for (size_t j = 1; j + 2 < scan_limit; j++) {
                if (data[j] == 0xFF && data[j+1] == 0xD8 && data[j+2] == 0xFF) {
                    soi_off = (int)j;
                    break;
                }
            }
        }
        if (soi_off < 0) continue;

        if (existing.count(cluster)) {
            for (auto &s : ctx.seeds) {
                if (s.start_cluster == cluster) {
                    s.source = SEED_BOTH;
                    break;
                }
            }
        } else {
            Seed s;
            s.start_cluster = cluster;
            s.soi_offset    = (uint16_t)soi_off;
            s.source        = SEED_SIGNATURE_SCAN;
            s.confidence    = (soi_off == 0) ? CONF_MEDIUM : CONF_LOW;
            s.jpeg_mode     = JPEG_UNKNOWN;
            s.filename      = "cluster_" + std::to_string(cluster) + ".jpg";
            ctx.seeds.push_back(std::move(s));
            existing.insert(cluster);
        }
    }
}

void seed_build(RecoveryContext &ctx)
{
    ctx.seeds.clear();
    ctx.seeds.reserve(8192);
    visited_dirs.clear();

    /* Phase 1: walk directory tree */
    walk_directory(ctx, ctx.disk.geo.root_cluster, "", 0);
    int dir_count = (int)ctx.seeds.size();

    /* Phase 2: extract EXIF thumbnails from dir-entry seeds */
    extract_thumbnails(ctx);

    /* Phase 3: signature scan */
    scan_signatures(ctx);
    int sig_new = (int)ctx.seeds.size() - dir_count;
    int thumb_count = 0;
    for (auto &s : ctx.seeds) if (s.has_thumbnail) thumb_count++;

    log_debug(ctx, "Seeds: %d from dir entries, %d new from signatures, %d thumbnails, %d total",
              dir_count, sig_new, thumb_count, (int)ctx.seeds.size());

    /* Sort: high confidence first, then by start_cluster */
    std::sort(ctx.seeds.begin(), ctx.seeds.end(), [](const Seed &a, const Seed &b) {
        if (a.confidence != b.confidence) return a.confidence > b.confidence;
        return a.start_cluster < b.start_cluster;
    });
}
