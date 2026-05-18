/*
 * scoring.cpp - Multi-signal candidate scoring (rebalanced)
 *
 * Key insight: on SD cards, sequential allocation is the norm. The scoring
 * should strongly favor sequential candidates and heavily penalize long-distance
 * jumps. Huffman validation is a pass/fail gate, not a quality signal.
 *
 * Tier weights rebalanced to reflect actual discriminative power:
 *   Huffman pass:     +20 (was +50, just a baseline admission fee)
 *   Spatial locality: +40 (was +10, THE primary signal for SD cards)
 *   DC boundary:      +30 (via multiplier, strong anti-stitching)
 *   FAT agreement:    +20 (was +10, strong when available)
 *   MCU progress:     +10 (was +15)
 *   DC continuity:    +10 (was +20, subsumed by boundary check)
 *   ff00 consistency: +5  (was +10, minor signal)
 *   Expected range:   +15 (NEW: penalize clusters far from expected position)
 * Max ~150, normalized to 0..1
 */
#include "sdrecov.h"
#include <cstdlib>
#include <cmath>

float score_candidate(const HuffResult &result,
                      const HuffCheckpoint &pre_state,
                      const HuffCheckpoint &post_state,
                      uint32_t candidate, uint32_t current, const Seed &seed,
                      const ClusterFeature *features, const FatTables &fat)
{
    if (!result.passed)
        return 0.0f;

    float score = 0.0f;

    /* Tier 1: Huffman validation passed (+20) - admission gate, not discriminator */
    score += 20.0f;

    /* Tier 2: Spatial locality (+40) - THE key signal for SD card recovery */
    uint32_t distance = (candidate > current)
                      ? (candidate - current) : (current - candidate);
    if (distance == 1)         score += 40.0f;  /* sequential - overwhelmingly likely correct */
    else if (distance == 2)    score += 30.0f;  /* skip-1 - common with small gaps */
    else if (distance <= 5)    score += 20.0f;  /* very near */
    else if (distance <= 64)   score += 8.0f;   /* nearby */
    else if (distance <= 1024) score += 2.0f;   /* same region */
    /* distance > 1024: no locality credit */

    /* Tier 3: FAT agreement (+20) */
    if (current < fat.count()) {
        if (fat.fat1[current] == fat.fat2[current] &&
            fat.fat1[current] == candidate)
            score += 20.0f; /* both FATs agree - very strong */
        else if (candidate == fat.merged[current])
            score += 12.0f; /* merged FAT points here */
        else if (candidate == fat.fat1[current] || candidate == fat.fat2[current])
            score += 8.0f;  /* one FAT agrees */
    }

    /* Tier 4: MCU progress (+10) */
    uint32_t new_mcus = post_state.mcu_count - pre_state.mcu_count;
    if (new_mcus > 100)     score += 10.0f;
    else if (new_mcus > 10) score += 7.0f;
    else if (new_mcus > 0)  score += 3.0f;

    /* Tier 5: DC predictor continuity (+10) */
    {
        int max_jump = 0;
        int num_comp = 0;
        for (int i = 0; i < MAX_COMPONENTS; i++) {
            if (pre_state.dc_pred[i] == 0 && post_state.dc_pred[i] == 0)
                continue;
            int jump = std::abs(post_state.dc_pred[i] - pre_state.dc_pred[i]);
            if (jump > max_jump) max_jump = jump;
            num_comp++;
        }
        if (num_comp > 0 && new_mcus > 0) {
            float avg_jump = (float)max_jump / std::max(new_mcus, 1u);
            if (avg_jump < 5.0f)       score += 10.0f;
            else if (avg_jump < 20.0f) score += 7.0f;
            else if (avg_jump < 50.0f) score += 3.0f;
        }
    }

    /* Tier 6: ff00 byte-stuffing rate consistency (+5) */
    if (features) {
        uint32_t cur_idx = current >= 2 ? current - 2 : 0;
        uint32_t cand_idx = candidate >= 2 ? candidate - 2 : 0;
        if (cur_idx < 1000000 && cand_idx < 1000000) {
            uint16_t cur_ff00 = features[cur_idx].ff00_count;
            uint16_t cand_ff00 = features[cand_idx].ff00_count;
            if (cur_ff00 > 5) {
                float ratio = (float)cand_ff00 / (float)cur_ff00;
                if (ratio > 0.7f && ratio < 1.4f)     score += 5.0f;
                else if (ratio > 0.5f && ratio < 2.0f) score += 2.0f;
            }
        }
    }

    /* Tier 7: Aggregate DC boundary coherence (+10) */
    {
        float coherence = boundary_coherence(pre_state, post_state, new_mcus);
        score += coherence * 10.0f;
    }

    /* Tier 8: Expected cluster range (+15) - penalize candidates far from
     * where the file's data should be on disk */
    if (seed.expected_clusters > 0 && seed.start_cluster >= 2) {
        /* Expected range: file clusters should be near start_cluster..start_cluster+expected_clusters */
        uint32_t expected_end = seed.start_cluster + seed.expected_clusters + 100; /* some slack */
        uint32_t expected_start = (seed.start_cluster > 100) ? seed.start_cluster - 100 : 2;

        if (candidate >= expected_start && candidate <= expected_end) {
            score += 15.0f; /* within expected range */
        } else {
            uint32_t range_dist = 0;
            if (candidate < expected_start) range_dist = expected_start - candidate;
            else range_dist = candidate - expected_end;

            if (range_dist < 500)       score += 5.0f;
            else if (range_dist < 5000) score += 1.0f;
            /* Far outside expected range: no credit */
        }
    }

    float raw = score / 150.0f;

    /* Apply first-MCU boundary coherence as final multiplier.
     * This is done in engine.cpp, not here - the bc value isn't
     * available in this function. The engine multiplies by (0.5 + bc*0.5). */

    return raw;
}

float evaluate_chain_quality(const RecoveryContext &ctx, const Seed &seed,
                              const std::vector<uint32_t> &clusters,
                              float avg_score, uint32_t mcus, bool complete,
                              float thumb_confidence)
{
    float eval = avg_score;

    if (complete) eval += 0.1f;

    if (thumb_confidence >= 0.8f)       eval += 0.2f;
    else if (thumb_confidence >= 0.5f)  eval += 0.05f;
    else if (thumb_confidence >= 0.0f)  eval -= 0.1f;

    if (seed.expected_clusters > 0) {
        float completeness = (float)clusters.size() / seed.expected_clusters;
        if (completeness > 0.9f)      eval += 0.1f;
        else if (completeness < 0.3f) eval -= 0.2f;
    }

    /* Spatial coherence bonus */
    if (clusters.size() > 3) {
        int sequential_pairs = 0;
        int total_pairs = (int)clusters.size() - 1;
        for (int i = 0; i < total_pairs; i++) {
            uint32_t diff = (clusters[i + 1] > clusters[i])
                          ? clusters[i + 1] - clusters[i]
                          : clusters[i] - clusters[i + 1];
            if (diff <= 2) sequential_pairs++;
        }
        float seq_ratio = (float)sequential_pairs / total_pairs;
        if (seq_ratio > 0.9f)      eval += 0.1f;
        else if (seq_ratio > 0.7f) eval += 0.05f;
        else if (seq_ratio < 0.3f) eval -= 0.15f;
    }

    return eval;
}
