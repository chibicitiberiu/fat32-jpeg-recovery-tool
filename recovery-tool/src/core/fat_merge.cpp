/*
 * fat_merge.cpp - Dual-FAT merge with bit-flip candidate enumeration
 */
#include "sdrecov.h"

static constexpr uint32_t FAT32_EOF_MIN = 0x0FFFFFF8;
static constexpr uint32_t FAT32_BAD_VAL = 0x0FFFFFF7;

static bool is_eof(uint32_t e)  { return e >= FAT32_EOF_MIN; }
static bool is_bad(uint32_t e)  { return e == FAT32_BAD_VAL; }
static bool is_free(uint32_t e) { return e == 0; }

static FatStatus classify(uint32_t entry, uint32_t max_cl)
{
    if (is_free(entry))                        return FAT_FREE;
    if (is_eof(entry))                         return FAT_EOF;
    if (is_bad(entry))                         return FAT_BAD;
    if (entry >= 2 && entry <= max_cl + 1)     return FAT_VALID;
    return FAT_CORRUPT;
}

void fat_merge(FatTables &fat, uint32_t total_clusters)
{
    uint32_t max_cl = total_clusters + 1;
    uint32_t n = fat.count();

    for (uint32_t i = 0; i < n; i++) {
        uint32_t  e1 = fat.fat1[i], e2 = fat.fat2[i];
        FatStatus c1 = classify(e1, max_cl);
        FatStatus c2 = classify(e2, max_cl);

        if (e1 == e2) {
            fat.merged[i] = e1;
            fat.status[i] = c1;
        } else if (c1 <= FAT_EOF && c2 == FAT_CORRUPT) {
            fat.merged[i] = e1; fat.status[i] = c1;
        } else if (c2 <= FAT_EOF && c1 == FAT_CORRUPT) {
            fat.merged[i] = e2; fat.status[i] = c2;
        } else if (is_free(e1) && !is_free(e2) && c2 != FAT_CORRUPT) {
            fat.merged[i] = e2; fat.status[i] = c2;
        } else if (is_free(e2) && !is_free(e1) && c1 != FAT_CORRUPT) {
            fat.merged[i] = e1; fat.status[i] = c1;
        } else if (c1 == FAT_VALID && c2 == FAT_VALID) {
            fat.merged[i] = e1; fat.status[i] = FAT_VALID; /* prefer FAT1 */
        } else {
            fat.merged[i] = e1; fat.status[i] = FAT_CORRUPT;
        }
    }
}

void fat_build_refcount(const FatTables &fat, uint32_t total_clusters,
                        std::vector<uint8_t> &refcount)
{
    refcount.assign(total_clusters + 2, 0);
    for (uint32_t i = 2; i < fat.count(); i++) {
        if (fat.status[i] == FAT_VALID) {
            uint32_t target = fat.merged[i];
            if (target >= 2 && target < (uint32_t)refcount.size() &&
                refcount[target] < 255)
                refcount[target]++;
        }
    }
}

int fat_bitflip_candidates(uint32_t entry, uint32_t max_cluster,
                            uint32_t *out, int max_out)
{
    int count = 0;
    for (int bit = 0; bit < 28 && count < max_out; bit++) {
        uint32_t flipped = (entry ^ (1u << bit)) & 0x0FFFFFFF;
        if (flipped >= 2 && flipped <= max_cluster + 1)
            out[count++] = flipped;
    }
    return count;
}
