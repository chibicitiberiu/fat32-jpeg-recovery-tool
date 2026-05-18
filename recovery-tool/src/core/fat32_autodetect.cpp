/*
 * fat32_autodetect.cpp - Automatic FAT32 geometry detection
 *
 * Reconstructs FAT32 geometry when boot sector is destroyed by scanning
 * for multiple independent signals: FAT table signatures, directory
 * structures, and file header alignment patterns.
 *
 * Works even when both FATs are completely destroyed.
 */
#include "sdrecov.h"
#include <cstring>
#include <algorithm>
#include <vector>
#include <cmath>

static uint32_t read32le(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

/* Valid FAT32 cluster sizes (powers of 2) */
static const uint32_t CLUSTER_SIZES[] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static const int NUM_CLUSTER_SIZES = 8;

/* ---- Pass 1: FAT Table Signature Scan ---- */

struct FatCandidate {
    uint64_t offset;
    uint32_t valid_entries;    /* count of valid FAT entries */
    uint32_t estimated_size;  /* estimated FAT size in bytes */
    float    quality;
};

static std::vector<FatCandidate> scan_fat_signatures(const uint8_t *data, size_t size,
                                                       uint64_t part_offset)
{
    std::vector<FatCandidate> candidates;

    /* Scan first 100MB from partition start at sector boundaries */
    uint64_t scan_end = std::min(part_offset + 100 * 1024 * 1024, (uint64_t)size - 8);

    for (uint64_t off = part_offset; off < scan_end; off += 512) {
        uint32_t e0 = read32le(data + off) & 0x0FFFFFFF;
        uint32_t e1 = read32le(data + off + 4) & 0x0FFFFFFF;

        /* FAT32 first entry: media descriptor (0x0FFFFFF8, 0x0FFFFFF0, 0x0FFFFFFC)
         * FAT32 second entry: EOF marker (0x0FFFFFFF) */
        bool e0_valid = (e0 == 0x0FFFFFF8 || e0 == 0x0FFFFFF0 || e0 == 0x0FFFFFFC);
        bool e1_valid = (e1 >= 0x0FFFFFF8);

        if (!e0_valid || !e1_valid) continue;

        /* Found potential FAT start. Find where it ends by looking for
         * the next FAT signature (start of FAT2) or a run of non-FAT data. */
        uint32_t valid = 0, total = 0;
        uint64_t pos = off + 8;
        uint64_t fat_end = 0;

        /* Method 1: Scan for the next FAT signature (FAT2 start).
         * FAT2 starts with the same F8/FF pattern at a sector-aligned offset. */
        for (uint64_t probe = off + 512; probe < off + 10 * 1024 * 1024 && probe + 8 < size; probe += 512) {
            uint32_t pe0 = read32le(data + probe) & 0x0FFFFFFF;
            uint32_t pe1 = read32le(data + probe + 4) & 0x0FFFFFFF;
            if ((pe0 == 0x0FFFFFF8 || pe0 == 0x0FFFFFF0 || pe0 == 0x0FFFFFFC) && pe1 >= 0x0FFFFFF8) {
                /* Found FAT2 signature. FAT1 size = probe - off */
                fat_end = probe;
                break;
            }
        }

        if (fat_end == 0) {
            /* Method 2: Count valid entries until we hit garbage */
            while (pos + 4 <= size && total < 500000) {
                uint32_t entry = read32le(data + pos) & 0x0FFFFFFF;
                total++;

                if (entry == 0 || (entry >= 2 && entry <= 0x0FFFFFEF) ||
                    entry >= 0x0FFFFFF7) {
                    valid++;
                } else {
                    uint32_t bad_run = 0;
                    while (pos + 4 <= size && bad_run < 50) {
                        uint32_t e = read32le(data + pos) & 0x0FFFFFFF;
                        if (e == 0 || (e >= 2 && e <= 0x0FFFFFEF) || e >= 0x0FFFFFF7) break;
                        bad_run++;
                        pos += 4;
                        total++;
                    }
                    if (bad_run >= 50) break;
                }
                pos += 4;
            }
            fat_end = off + total * 4;
        } else {
            /* Validate entries within the known FAT range */
            total = (fat_end - off) / 4;
            for (uint64_t p = off + 8; p + 4 <= fat_end; p += 4) {
                uint32_t entry = read32le(data + p) & 0x0FFFFFFF;
                if (entry == 0 || (entry >= 2 && entry <= 0x0FFFFFEF) || entry >= 0x0FFFFFF7)
                    valid++;
            }
        }

        float quality = (total > 2) ? (float)valid / (total - 2) : 0;
        uint32_t fat_size_bytes = (uint32_t)(fat_end - off);

        if (quality > 0.5f && fat_size_bytes >= 512) {
            FatCandidate fc;
            fc.offset = off;
            fc.valid_entries = valid;
            fc.estimated_size = fat_size_bytes;
            fc.quality = quality;
            candidates.push_back(fc);
            /* Trace detection (visible at -vvv via log_debug, but we don't have ctx here) */
        }
    }

    return candidates;
}

/* ---- Pass 2: Directory Structure Scan ---- */

struct DirCandidate {
    uint64_t offset;
    int      valid_entries;
    float    quality;
};

static bool is_valid_dir_entry(const uint8_t *e)
{
    /* Check if 32 bytes look like a FAT directory entry */
    uint8_t first = e[0];
    uint8_t attr = e[11];

    /* End of directory */
    if (first == 0x00) return true;

    /* Deleted entry */
    if (first == 0xE5) return true;

    /* LFN entry */
    if (attr == 0x0F) {
        /* LFN: first byte is sequence number (0x01-0x7F) */
        return (first >= 0x01 && first <= 0x7F);
    }

    /* Regular entry: first byte must be printable ASCII (but not 0x20 as first char) */
    if (first < 0x20 || first == 0x7F) return false;

    /* Attribute byte: valid values are 0x00-0x3F (combinations of RDONLY, HIDDEN, etc.) */
    if (attr > 0x3F && attr != 0x0F) return false;

    return true;
}

static std::vector<DirCandidate> scan_directory_structures(const uint8_t *data, size_t size,
                                                            uint64_t part_offset)
{
    std::vector<DirCandidate> candidates;

    /* Scan at 512-byte boundaries looking for runs of valid dir entries */
    uint64_t scan_end = std::min(part_offset + (uint64_t)size, (uint64_t)size);

    for (uint64_t off = part_offset; off + 512 <= scan_end; off += 512) {
        int valid = 0;
        int total = 16; /* check first 16 entries (512 bytes) */

        for (int i = 0; i < total && off + (i + 1) * 32 <= size; i++) {
            if (is_valid_dir_entry(data + off + i * 32))
                valid++;
        }

        float quality = (float)valid / total;

        /* A real directory cluster has nearly all valid entries */
        if (quality >= 0.8f && valid >= 10) {
            DirCandidate dc;
            dc.offset = off;
            dc.valid_entries = valid;
            dc.quality = quality;
            candidates.push_back(dc);

            /* Skip ahead to avoid duplicates within the same directory cluster */
            off += 4096 - 512;
        }
    }

    return candidates;
}

/* ---- Pass 3: Cluster Size Detection via File Header Alignment ---- */

struct AlignmentScore {
    uint32_t cluster_size;
    uint64_t data_offset;
    int      aligned_count;
    float    score;
};

static std::vector<uint64_t> find_file_headers(const uint8_t *data, size_t size,
                                                 uint64_t start_offset)
{
    std::vector<uint64_t> headers;

    for (uint64_t off = start_offset; off + 4 < size; off++) {
        /* JPEG SOI: FF D8 FF */
        if (data[off] == 0xFF && data[off+1] == 0xD8 && data[off+2] == 0xFF) {
            headers.push_back(off);
            off += 2; /* skip past this header */
            continue;
        }
        /* PNG: 89 50 4E 47 */
        if (data[off] == 0x89 && data[off+1] == 0x50 && data[off+2] == 0x4E && data[off+3] == 0x47) {
            headers.push_back(off);
            continue;
        }
        /* PDF: 25 50 44 46 */
        if (data[off] == 0x25 && data[off+1] == 0x50 && data[off+2] == 0x44 && data[off+3] == 0x46) {
            headers.push_back(off);
            continue;
        }
        /* ZIP/DOCX: 50 4B 03 04 */
        if (data[off] == 0x50 && data[off+1] == 0x4B && data[off+2] == 0x03 && data[off+3] == 0x04) {
            headers.push_back(off);
            continue;
        }
    }

    return headers;
}

static AlignmentScore find_best_alignment(const std::vector<uint64_t> &headers,
                                           const std::vector<DirCandidate> &dir_candidates,
                                           uint64_t part_offset, size_t image_size)
{
    AlignmentScore best = {0, 0, 0, 0};

    /* Generate candidate data offsets from directory locations */
    std::vector<uint64_t> candidate_data_offsets;
    for (auto &dc : dir_candidates) {
        /* If this is the root directory (cluster 2), data_offset = dc.offset */
        candidate_data_offsets.push_back(dc.offset);
    }

    /* Also try common reserved sector counts: 32, 36, 64 sectors */
    /* FAT size depends on cluster size, so we just try offsets */
    for (uint32_t reserved = 16; reserved <= 128; reserved += 4) {
        uint64_t off = part_offset + reserved * 512;
        /* For each FAT size guess (64KB to 1MB) */
        for (uint32_t fat_kb = 64; fat_kb <= 2048; fat_kb += 64) {
            candidate_data_offsets.push_back(off + 2 * fat_kb * 1024);
        }
    }

    /* Deduplicate and sort */
    std::sort(candidate_data_offsets.begin(), candidate_data_offsets.end());
    candidate_data_offsets.erase(
        std::unique(candidate_data_offsets.begin(), candidate_data_offsets.end()),
        candidate_data_offsets.end());

    for (int cs_idx = 0; cs_idx < NUM_CLUSTER_SIZES; cs_idx++) {
        uint32_t cs = CLUSTER_SIZES[cs_idx];

        for (uint64_t data_off : candidate_data_offsets) {
            if (data_off >= image_size || data_off < part_offset) continue;

            int aligned = 0;
            for (uint64_t h : headers) {
                if (h < data_off) continue;
                uint64_t rel = h - data_off;
                if (rel % cs == 0) aligned++;
            }

            /* Score: aligned count weighted by cluster size preference.
             * Larger cluster sizes are more common and more likely correct.
             * A 512-byte alignment trivially matches everything, so penalize it.
             * Score = aligned_count * log2(cluster_size) to prefer larger sizes. */
            float weighted = (float)aligned * log2f((float)cs);
            if (weighted > best.score) {
                best.cluster_size = cs;
                best.data_offset = data_off;
                best.aligned_count = aligned;
                best.score = weighted;
            }
        }
    }

    return best;
}

/* ---- Pass 4: Geometry Solver ---- */

Fat32AutodetectResult fat32_autodetect(const uint8_t *image, size_t image_size,
                                        uint64_t partition_offset)
{
    Fat32AutodetectResult result = {};

    /* Pass 1: Look for FAT tables */
    auto fat_candidates = scan_fat_signatures(image, image_size, partition_offset);

    /* Pass 2: Look for directory structures */
    auto dir_candidates = scan_directory_structures(image, image_size, partition_offset);

    /* Pass 3: Find file headers and determine cluster alignment */
    auto file_headers = find_file_headers(image, image_size, partition_offset);

    AlignmentScore alignment = find_best_alignment(file_headers, dir_candidates,
                                                     partition_offset, image_size);

    /* Solve geometry from available evidence */
    auto &geo = result.geo;
    float confidence = 0;

    /* Common defaults */
    geo.bytes_per_sector = 512;
    geo.partition_offset = partition_offset;
    geo.root_cluster = 2; /* FAT32 root dir is always cluster 2 */

    /* Start with alignment-derived values (most reliable when boot sector is gone) */
    if (alignment.aligned_count >= 3) {
        geo.bytes_per_cluster = alignment.cluster_size;
        geo.sectors_per_cluster = alignment.cluster_size / 512;
        geo.data_offset = alignment.data_offset;
        geo.total_clusters = (image_size - geo.data_offset) / geo.bytes_per_cluster;
        confidence += 0.4f;
    }

    /* Enhance with FAT table info if available */
    if (!fat_candidates.empty()) {
        /* Pick the FIRST FAT candidate (lowest offset) - FAT1 is always before FAT2.
         * If there are two FAT signatures, the first is FAT1 and the second is FAT2.
         * The size of FAT1 is precisely the distance to FAT2. */
        std::sort(fat_candidates.begin(), fat_candidates.end(),
                  [](const FatCandidate &a, const FatCandidate &b) {
                      return a.offset < b.offset;
                  });

        /* If we have two candidates, FAT1 size = FAT2 offset - FAT1 offset */
        if (fat_candidates.size() >= 2) {
            fat_candidates[0].estimated_size = (uint32_t)(fat_candidates[1].offset - fat_candidates[0].offset);
        }
        auto &best_fat = fat_candidates[0];
        geo.fat1_offset = best_fat.offset;
        geo.sectors_per_fat = best_fat.estimated_size / 512;
        geo.num_fats = 1;

        /* Check for FAT2 */
        uint64_t expected_fat2 = geo.fat1_offset + (uint64_t)geo.sectors_per_fat * 512;
        if (expected_fat2 + 8 < image_size) {
            uint32_t f2e0 = read32le(image + expected_fat2) & 0x0FFFFFFF;
            uint32_t f2e1 = read32le(image + expected_fat2 + 4) & 0x0FFFFFFF;
            if ((f2e0 == 0x0FFFFFF8 || f2e0 == 0x0FFFFFF0) && f2e1 >= 0x0FFFFFF8) {
                geo.fat2_offset = expected_fat2;
                geo.num_fats = 2;
            }
        }

        /* Derive data_offset from FATs. This is more reliable than alignment
         * because it's computed directly from known FAT positions. */
        uint64_t fat_derived_data_off = geo.fat1_offset + (uint64_t)geo.num_fats * geo.sectors_per_fat * 512;
        geo.data_offset = fat_derived_data_off; /* always prefer FAT-derived offset */

        /* Use FAT entry count to constrain cluster size.
         * total_fat_entries ~= total_clusters + 2.
         * data_bytes = image_size - data_offset.
         * cluster_size = data_bytes / total_clusters. */
        uint32_t fat_entries = best_fat.estimated_size / 4;
        if (fat_entries > 2 && geo.data_offset > 0) {
            uint64_t data_bytes = image_size - geo.data_offset;
            uint64_t estimated_cs = data_bytes / (fat_entries - 2);
            /* Round to nearest valid cluster size (allow 5% tolerance above) */
            for (int i = NUM_CLUSTER_SIZES - 1; i >= 0; i--) {
                if (estimated_cs >= CLUSTER_SIZES[i] * 0.95) {
                    geo.bytes_per_cluster = CLUSTER_SIZES[i];
                    geo.sectors_per_cluster = CLUSTER_SIZES[i] / 512;
                    geo.total_clusters = data_bytes / geo.bytes_per_cluster;
                    break;
                }
            }
        }

        geo.reserved_sectors = (geo.fat1_offset - partition_offset) / 512;
        confidence += 0.3f * best_fat.quality;
    } else {
        /* No FATs found - derive what we can */
        geo.fat1_offset = 0;
        geo.fat2_offset = 0;
        geo.num_fats = 0;
        geo.sectors_per_fat = 0;
        if (geo.data_offset > partition_offset) {
            geo.reserved_sectors = (geo.data_offset - partition_offset) / 512;
        } else {
            geo.reserved_sectors = 32; /* common default */
        }
    }

    /* Enhance with directory info */
    if (!dir_candidates.empty()) {
        /* The first directory cluster should be cluster 2 (root dir).
         * If we don't have data_offset yet, derive from directory location. */
        if (geo.data_offset == 0 && geo.bytes_per_cluster > 0) {
            /* cluster 2 is the first data cluster, so:
             * data_offset = dir_offset - (2 - 2) * cluster_size = dir_offset */
            geo.data_offset = dir_candidates[0].offset;
            geo.total_clusters = (image_size - geo.data_offset) / geo.bytes_per_cluster;
        }
        confidence += 0.2f;
    }

    /* Final consistency checks */
    if (geo.bytes_per_cluster == 0 || geo.data_offset == 0) {
        result.valid = false;
        result.confidence = 0;
        return result;
    }

    geo.total_sectors = (image_size - partition_offset) / 512;
    if (geo.total_clusters == 0) {
        geo.total_clusters = (image_size - geo.data_offset) / geo.bytes_per_cluster;
    }

    /* Validate root directory at cluster 2 */
    uint64_t root_off = geo.data_offset; /* cluster 2 = first data cluster */
    if (root_off + 32 < image_size && is_valid_dir_entry(image + root_off)) {
        confidence += 0.1f;
    }

    result.valid = (confidence >= 0.3f);
    result.confidence = std::min(confidence, 1.0f);
    return result;
}
