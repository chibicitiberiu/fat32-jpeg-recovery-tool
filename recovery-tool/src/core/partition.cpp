/*
 * partition.cpp - MBR partition table parser
 */
#include "sdrecov.h"
#include <cstring>

static constexpr uint16_t MBR_SIG = 0xAA55;

static uint32_t read32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8)
         | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

int partition_detect(const uint8_t *image, size_t size,
                     std::vector<PartitionEntry> &entries)
{
    entries.clear();
    if (size < 512) return -1;

    uint16_t sig = image[510] | (image[511] << 8);
    if (sig != MBR_SIG) return -1;

    for (int i = 0; i < 4; i++) {
        const uint8_t *e = image + 0x1BE + i * 16;
        PartitionEntry pe;
        pe.status       = e[0];
        pe.type         = e[4];
        pe.start_lba    = read32(e + 8);
        pe.size_sectors = read32(e + 12);
        entries.push_back(pe);
    }
    return 0;
}

uint64_t partition_get_offset(const uint8_t *image, size_t size, int partition_num)
{
    std::vector<PartitionEntry> entries;

    if (partition_detect(image, size, entries) != 0)
        return 0; /* no MBR, treat as bare partition */

    if (partition_num > 0) {
        if (partition_num < 1 || partition_num > 4) {
            log_error("partition number must be 1-4, got %d", partition_num);
            return UINT64_MAX;
        }
        auto &pe = entries[partition_num - 1];
        if (pe.type == 0) {
            log_error("partition %d is empty", partition_num);
            return UINT64_MAX;
        }
        return uint64_t(pe.start_lba) * SECTOR_SIZE;
    }

    /* Auto-detect first FAT32 partition (type 0x0B or 0x0C) */
    for (auto &pe : entries)
        if (pe.type == 0x0B || pe.type == 0x0C)
            return uint64_t(pe.start_lba) * SECTOR_SIZE;

    /* Fall back to first non-empty */
    for (auto &pe : entries)
        if (pe.type != 0)
            return uint64_t(pe.start_lba) * SECTOR_SIZE;

    return 0; /* empty table, bare image */
}
