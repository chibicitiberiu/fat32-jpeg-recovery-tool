/*
 * ftl_block.cpp - Pass 2: FTL mapping failure corruption.
 *
 * Models block-level failures from the Flash Translation Layer:
 * - Block swaps: entire aligned regions contain coherent but wrong data
 * - Zero fills: controller returned blank page on failed read
 * - Wrong-data fills: corrupted mapping table points to wrong physical block
 *
 * These are aligned to the FTL allocation group size (typically 4MB for SDHC).
 * The FTL operates below the filesystem, so block boundaries don't align
 * with FAT32 cluster boundaries.
 */
#include "corrsim.h"
#include <cstring>
#include <algorithm>
#include <set>

bool corrupt_ftl(SimContext &ctx, uint8_t *data, size_t size,
                 uint64_t region_start, uint64_t region_end)
{
    auto &cfg = ctx.cfg.ftl;
    if (cfg.swap_frac <= 0 && cfg.zero_frac <= 0 && cfg.wrong_data_frac <= 0)
        return true;

    uint32_t block_size = cfg.block_size;
    uint64_t total_blocks = size / block_size;  // only full blocks

    if (total_blocks < 4) {
        log_info(ctx, "Image too small for FTL block corruption "
                 "(%lu blocks of %uB)", (unsigned long)total_blocks, block_size);
        return true;
    }

    // Build a shuffled list of block indices within the target region
    std::vector<uint32_t> block_indices;
    for (uint32_t i = 0; i < total_blocks; i++) {
        uint64_t bstart = (uint64_t)i * block_size;
        uint64_t bend = bstart + block_size;
        if (bend > region_start && bstart < region_end)
            block_indices.push_back(i);
    }

    uint32_t eligible = (uint32_t)block_indices.size();
    if (eligible < 4) {
        log_info(ctx, "Too few eligible FTL blocks in region (%u)", eligible);
        return true;
    }

    log_progress(ctx, "  Pass 2: FTL block corruption (%u eligible of %lu blocks, %uMB each)",
                 eligible, (unsigned long)total_blocks, block_size / 1048576);

    rng_shuffle_u32(ctx, block_indices);

    uint32_t swap_count = std::max(1u, (uint32_t)(eligible * cfg.swap_frac));
    uint32_t zero_count = std::max(1u, (uint32_t)(eligible * cfg.zero_frac));
    uint32_t wrong_count = std::max(1u, (uint32_t)(eligible * cfg.wrong_data_frac));

    // Don't corrupt more than 40% of eligible blocks
    uint32_t max_corrupt = eligible * 40 / 100;
    uint32_t total_needed = swap_count * 2 + zero_count + wrong_count;
    if (total_needed > max_corrupt) {
        double scale = (double)max_corrupt / total_needed;
        swap_count  = std::max(1u, (uint32_t)(swap_count * scale));
        zero_count  = std::max(1u, (uint32_t)(zero_count * scale));
        wrong_count = std::max(1u, (uint32_t)(wrong_count * scale));
    }

    uint32_t idx = 0;

    // Zero-filled blocks
    for (uint32_t i = 0; i < zero_count && idx < (uint32_t)block_indices.size(); i++) {
        uint32_t b = block_indices[idx++];
        uint64_t offset = (uint64_t)b * block_size;
        memset(data + offset, 0, block_size);

        ctx.truth.blocks_zeroed++;
        char detail[128];
        snprintf(detail, sizeof(detail), "block %u zeroed", b);
        ctx.truth.mutations.push_back({"block_zero", offset, block_size, detail});

        log_detail(ctx, "FTL zero: block %u (offset %lu)", b, (unsigned long)offset);
    }

    // Block swaps
    for (uint32_t i = 0; i < swap_count && idx + 1 < (uint32_t)block_indices.size(); i++) {
        uint32_t ba = block_indices[idx++];
        uint32_t bb = block_indices[idx++];
        uint64_t off_a = (uint64_t)ba * block_size;
        uint64_t off_b = (uint64_t)bb * block_size;

        // Swap contents using a temporary buffer
        std::vector<uint8_t> tmp(block_size);
        memcpy(tmp.data(), data + off_a, block_size);
        memcpy(data + off_a, data + off_b, block_size);
        memcpy(data + off_b, tmp.data(), block_size);

        ctx.truth.blocks_swapped++;
        char detail[128];
        snprintf(detail, sizeof(detail), "blocks %u <-> %u swapped", ba, bb);
        ctx.truth.mutations.push_back({"block_swap", off_a, block_size, detail});

        log_detail(ctx, "FTL swap: block %u <-> %u", ba, bb);
    }

    // Wrong-data blocks (copy data from random other block)
    for (uint32_t i = 0; i < wrong_count && idx < (uint32_t)block_indices.size(); i++) {
        uint32_t target = block_indices[idx++];
        uint32_t source = rng_int(ctx, 0, (int)total_blocks - 1);
        if (source == target) source = (source + 1) % total_blocks;

        uint64_t off_target = (uint64_t)target * block_size;
        uint64_t off_source = (uint64_t)source * block_size;
        memcpy(data + off_target, data + off_source, block_size);

        ctx.truth.blocks_wrong_data++;
        char detail[128];
        snprintf(detail, sizeof(detail), "block %u overwritten with data from block %u",
                 target, source);
        ctx.truth.mutations.push_back({"block_wrong_data", off_target, block_size, detail});

        log_detail(ctx, "FTL wrong data: block %u <- block %u", target, source);
    }

    log_progress(ctx, "  FTL: %u zeroed, %u swapped, %u wrong-data",
                 ctx.truth.blocks_zeroed, ctx.truth.blocks_swapped,
                 ctx.truth.blocks_wrong_data);

    return true;
}
