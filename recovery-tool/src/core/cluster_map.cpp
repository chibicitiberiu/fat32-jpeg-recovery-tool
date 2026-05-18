/*
 * cluster_map.cpp - Cluster classification by content analysis
 */
#include "sdrecov.h"
#include <cmath>
#include <cstring>

static float shannon_entropy(const uint8_t *data, size_t len)
{
    uint32_t counts[256] = {};
    for (size_t i = 0; i < len; i++)
        counts[data[i]]++;

    float entropy = 0.0f;
    float inv = 1.0f / float(len);
    for (int i = 0; i < 256; i++) {
        if (counts[i] > 0) {
            float p = float(counts[i]) * inv;
            entropy -= p * log2f(p);
        }
    }
    return entropy;
}

static uint16_t count_ff00(const uint8_t *data, size_t len)
{
    uint16_t n = 0;
    for (size_t i = 0; i + 1 < len; i++)
        if (data[i] == 0xFF && data[i + 1] == 0x00)
            n++;
    return n;
}

static void scan_markers(const uint8_t *data, size_t len, ClusterFeature &cf)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] != 0xFF) continue;
        switch (data[i + 1]) {
        case 0xD8: cf.flags |= CF_HAS_SOI; break;
        case 0xD9: cf.flags |= CF_HAS_EOI; break;
        case 0xDA: cf.flags |= CF_HAS_SOS; break;
        case 0xDB: cf.flags |= CF_HAS_DQT; break;
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        case 0xD4: case 0xD5: case 0xD6: case 0xD7:
            cf.flags |= CF_HAS_RST; break;
        }
    }

    /* MP4 ftyp check */
    if (len >= 8) {
        for (size_t i = 0; i + 7 < len; i += 4) {
            if (data[i+4]=='f' && data[i+5]=='t' && data[i+6]=='y' && data[i+7]=='p') {
                cf.content_type = CTYPE_MP4_HEADER;
                return;
            }
        }
    }
}

static void classify(ClusterFeature &cf)
{
    if (cf.content_type == CTYPE_MP4_HEADER) return;
    if (cf.flags & CF_IS_ZERO)     { cf.content_type = CTYPE_EMPTY; return; }
    if (cf.flags & CF_HAS_SOI)     { cf.content_type = CTYPE_JPEG_HEADER; return; }
    if (cf.entropy > 7.0f && cf.ff00_count > 3) { cf.content_type = CTYPE_JPEG_SCAN; return; }
    if (cf.entropy > 7.5f && cf.ff00_count <= 1) { cf.content_type = CTYPE_UNKNOWN; return; }
    if (cf.entropy > 1.0f)         { cf.content_type = CTYPE_NON_JPEG; return; }
    cf.content_type = CTYPE_UNKNOWN;
}

void cluster_map_build(const DiskImage &disk, std::vector<ClusterFeature> &map)
{
    uint32_t n = disk.geo.total_clusters;
    map.resize(n);

    uint32_t bpc = disk.geo.bytes_per_cluster;

    for (uint32_t i = 0; i < n; i++) {
        auto &cf = map[i];
        cf = {};

        const uint8_t *data = disk.cluster_ptr(i + 2);
        if (!data) { cf.content_type = CTYPE_BAD_SECTOR; continue; }

        /* All-zero check */
        bool all_zero = true;
        for (uint32_t j = 0; j < bpc; j++)
            if (data[j] != 0) { all_zero = false; break; }
        if (all_zero) cf.flags |= CF_IS_ZERO;

        cf.entropy    = shannon_entropy(data, bpc);
        cf.ff00_count = count_ff00(data, bpc);
        scan_markers(data, bpc, cf);
        classify(cf);
    }
}
