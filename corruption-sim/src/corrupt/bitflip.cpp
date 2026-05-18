/*
 * bitflip.cpp - Pass 1: NAND retention bit-level corruption.
 *
 * Models biased bit flips with spatial clustering by erase block.
 * Uses geometric distribution for O(flips) performance instead of O(bits).
 *
 * Physics basis:
 * - Retention errors: charge leak from floating-gate cells, 0->1 dominant
 * - Read disturb errors: pass-through voltage injection, 1->0 dominant
 * - Combined bias typically ~80% in the 0->1 direction
 * - Degraded blocks (high P/E cycles) have 10x higher BER
 * - MSB pages in MLC NAND have 2-3x higher error rates
 */
#include "corrsim.h"
#include <cstring>
#include <cstdio>
#include <set>

bool corrupt_bitflip(SimContext &ctx, uint8_t *data, size_t size,
                     uint64_t region_start, uint64_t region_end)
{
    auto &cfg = ctx.cfg.bitflip;
    if (cfg.ber <= 0) return true;

    log_progress(ctx, "  Pass 1: Bit-flip corruption (BER=%.1e, bias=%.0f%% 0->1)",
                 cfg.ber, cfg.bias_0to1 * 100);

    uint32_t erase_block_size = cfg.erase_block_size;
    uint64_t total_blocks = (size + erase_block_size - 1) / erase_block_size;

    // Determine which erase blocks are "degraded"
    std::set<uint64_t> degraded_blocks;
    for (uint64_t b = 0; b < total_blocks; b++) {
        if (rng_bernoulli(ctx, cfg.degraded_frac))
            degraded_blocks.insert(b);
    }

    log_info(ctx, "Degraded erase blocks: %zu / %lu (%.1f%%)",
             degraded_blocks.size(), (unsigned long)total_blocks,
             100.0 * degraded_blocks.size() / total_blocks);

    uint64_t total_flips = 0;
    uint64_t flips_0to1 = 0;
    uint64_t flips_1to0 = 0;

    uint64_t effective_end = std::min((uint64_t)size, region_end);

    // Process each erase block
    for (uint64_t b = 0; b < total_blocks; b++) {
        uint64_t block_start = b * erase_block_size;
        uint64_t block_end = std::min(block_start + erase_block_size, size);

        // Skip blocks outside the target region
        if (block_end <= region_start || block_start >= effective_end)
            continue;
        uint64_t block_bytes = block_end - block_start;
        uint64_t block_bits = block_bytes * 8;

        bool is_degraded = degraded_blocks.count(b) > 0;
        double effective_ber = cfg.ber * (is_degraded ? cfg.degraded_mult : 1.0);

        // Use geometric distribution to jump between flip positions
        // This gives O(flips) performance instead of O(bits)
        uint64_t bit_pos = rng_geometric(ctx, effective_ber);

        while (bit_pos < block_bits) {
            uint64_t abs_byte = block_start + bit_pos / 8;
            int bit_idx = bit_pos % 8;

            if (abs_byte < size) {
                uint8_t mask = 1 << bit_idx;
                bool current_bit = (data[abs_byte] & mask) != 0;

                // Apply directional bias
                // If bit is 0: flip with probability proportional to bias_0to1
                // If bit is 1: flip with probability proportional to (1 - bias_0to1)
                bool do_flip;
                if (current_bit == false) {
                    // 0->1 direction (retention error)
                    do_flip = rng_bernoulli(ctx, cfg.bias_0to1);
                } else {
                    // 1->0 direction (read disturb)
                    do_flip = rng_bernoulli(ctx, 1.0 - cfg.bias_0to1);
                }

                // MSB page bias: within degraded blocks, every other 8KB
                // page gets 2x error rate. We simulate this by only
                // applying the flip sometimes for LSB pages.
                if (is_degraded && cfg.msb_bias) {
                    uint64_t page_in_block = (abs_byte - block_start) / 8192;
                    bool is_msb_page = (page_in_block % 2) == 1;
                    if (!is_msb_page && !rng_bernoulli(ctx, 0.4)) {
                        // LSB page in degraded block: reduce flips
                        do_flip = false;
                    }
                }

                if (do_flip) {
                    data[abs_byte] ^= mask;
                    total_flips++;
                    if (!current_bit) flips_0to1++;
                    else flips_1to0++;

                    if (ctx.cfg.full_manifest) {
                        char detail[128];
                        snprintf(detail, sizeof(detail),
                                 "bit %d of byte, %s, block %lu%s",
                                 bit_idx, current_bit ? "1->0" : "0->1",
                                 (unsigned long)b, is_degraded ? " (degraded)" : "");
                        ctx.truth.mutations.push_back({
                            "bitflip", abs_byte, 1, detail
                        });
                    }

                    log_debug(ctx, "Flip @ offset %lu bit %d: %s (block %lu%s)",
                              (unsigned long)abs_byte, bit_idx,
                              current_bit ? "1->0" : "0->1",
                              (unsigned long)b,
                              is_degraded ? ", degraded" : "");
                }
            }

            // Jump to next flip position
            uint64_t gap = rng_geometric(ctx, effective_ber);
            if (gap == 0) gap = 1;
            bit_pos += gap;
        }
    }

    ctx.truth.total_bits_flipped += total_flips;

    log_progress(ctx, "  Bit flips applied: %lu total (%lu 0->1, %lu 1->0)",
                 (unsigned long)total_flips,
                 (unsigned long)flips_0to1,
                 (unsigned long)flips_1to0);
    log_info(ctx, "Effective rate: %.2e per bit (%.1f flips/MB)",
             (double)total_flips / (size * 8.0),
             (double)total_flips / (size / 1048576.0));

    return true;
}
