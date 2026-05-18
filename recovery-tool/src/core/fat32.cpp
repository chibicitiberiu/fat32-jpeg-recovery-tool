/*
 * fat32.cpp - FAT32 geometry parser and FAT table reader
 * Parses boot sector (with backup sector 6 fallback).
 */
#include "sdrecov.h"
#include <cstring>

static uint16_t read16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t read32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static bool try_parse_boot_sector(const uint8_t *data, size_t data_size,
                                   uint64_t offset, Fat32Geometry &geo)
{
    if (offset + 512 > data_size) return false;
    const uint8_t *bs = data + offset;

    /* Jump instruction check */
    if (bs[0] != 0xEB && bs[0] != 0xE9) return false;

    uint16_t bps = read16(bs + 11);
    if (bps == 0 || bps > 4096) return false;

    uint8_t spc = bs[13];
    if (spc == 0) return false;

    uint8_t nfats = bs[16];
    if (nfats == 0 || nfats > 2) return false;

    uint32_t spf = read32(bs + 36);
    if (spf == 0) return false;

    uint16_t reserved = read16(bs + 14);
    uint32_t root_cl  = read32(bs + 44);
    uint32_t total    = read32(bs + 32);
    if (total == 0) total = read16(bs + 19);

    uint32_t data_start = reserved + nfats * spf;
    uint32_t data_secs  = total - data_start;
    uint32_t clusters   = data_secs / spc;

    if (clusters < 65525) return false; /* FAT32 needs >= 65525 */

    geo.bytes_per_sector    = bps;
    geo.sectors_per_cluster = spc;
    geo.bytes_per_cluster   = bps * spc;
    geo.reserved_sectors    = reserved;
    geo.num_fats            = nfats;
    geo.sectors_per_fat     = spf;
    geo.root_cluster        = root_cl;
    geo.total_sectors       = total;
    geo.total_clusters      = clusters;
    geo.partition_offset    = offset;
    geo.fat1_offset         = offset + uint64_t(reserved) * bps;
    geo.fat2_offset         = geo.fat1_offset + uint64_t(spf) * bps;
    geo.data_offset         = offset + uint64_t(data_start) * bps;
    return true;
}

bool fat32_parse(DiskImage &disk, uint64_t partition_offset)
{
    /* Try primary boot sector */
    if (try_parse_boot_sector(disk.data(), disk.size(), partition_offset, disk.geo))
        return true;

    /* Try backup at sector 6 */
    uint64_t backup = partition_offset + 6 * SECTOR_SIZE;
    if (try_parse_boot_sector(disk.data(), disk.size(), backup, disk.geo)) {
        /* Fix offsets to point at actual partition, not backup location */
        auto &g = disk.geo;
        g.partition_offset = partition_offset;
        g.fat1_offset = partition_offset + uint64_t(g.reserved_sectors) * g.bytes_per_sector;
        g.fat2_offset = g.fat1_offset + uint64_t(g.sectors_per_fat) * g.bytes_per_sector;
        uint32_t ds = g.reserved_sectors + g.num_fats * g.sectors_per_fat;
        g.data_offset = partition_offset + uint64_t(ds) * g.bytes_per_sector;
        return true;
    }
    /* Try autodetection as last resort */
    auto detected = fat32_autodetect(disk.data(), disk.size(), partition_offset);
    if (detected.valid) {
        disk.geo = detected.geo;
        return true;
    }

    return false;
}

bool fat32_read_tables(const DiskImage &disk, FatTables &fat)
{
    uint32_t count = disk.geo.total_clusters + 2;
    fat.resize(count);

    size_t fat_bytes = size_t(count) * 4;

    if (disk.geo.fat1_offset > 0 && disk.geo.fat1_offset + fat_bytes <= disk.size()) {
        auto src = reinterpret_cast<const uint32_t *>(disk.data() + disk.geo.fat1_offset);
        for (uint32_t i = 0; i < count; i++)
            fat.fat1[i] = src[i] & 0x0FFFFFFF;
    }

    if (disk.geo.fat2_offset > 0 && disk.geo.fat2_offset + fat_bytes <= disk.size()) {
        auto src = reinterpret_cast<const uint32_t *>(disk.data() + disk.geo.fat2_offset);
        for (uint32_t i = 0; i < count; i++)
            fat.fat2[i] = src[i] & 0x0FFFFFFF;
    }
    return true;
}
