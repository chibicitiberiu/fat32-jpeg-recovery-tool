/*
 * candidates.cpp - Candidate next-cluster enumeration with priority ranking
 *
 * For a given current cluster in a chain, enumerate possible next clusters
 * in priority order: FAT-based > sequential > bit-flip > nearby JPEG clusters.
 * Pre-filter with ff00_count density to reject obvious mismatches cheaply.
 */
#include "sdrecov.h"
#include <cstdlib>
#include <algorithm>

struct Candidate {
    uint32_t cluster;
    float    priority;
};

/*
 * Enumerate candidates for the next cluster after 'current'.
 * Writes to out_candidates/out_priorities, returns count.
 */
int enumerate_candidates(uint32_t current, const RecoveryContext &ctx,
                         const Seed &seed, int chain_length,
                         uint32_t *out_candidates, float *out_priorities,
                         int max_candidates)
{
    std::vector<Candidate> cands;
    cands.reserve(max_candidates * 2);

    auto &fat = ctx.fat;
    uint32_t max_cl = ctx.disk.geo.total_clusters + 1;
    auto &cmap = ctx.cluster_map;

    auto add = [&](uint32_t cl, float prio) {
        if (cl < 2 || cl > max_cl) return;
        /* Dedup */
        for (auto &c : cands)
            if (c.cluster == cl) { c.priority = std::max(c.priority, prio); return; }
        cands.push_back({cl, prio});
    };

    /* Priority 1: FAT merged entry (100) */
    uint32_t merged = fat.merged[current];
    if (fat.status[current] == FAT_VALID)
        add(merged, 100.0f);

    /* Priority 1b: FAT1 and FAT2 if different from merged (90) */
    if (fat.fat1[current] != merged && fat.fat1[current] >= 2 && fat.fat1[current] <= max_cl)
        add(fat.fat1[current], 90.0f);
    if (fat.fat2[current] != merged && fat.fat2[current] >= 2 && fat.fat2[current] <= max_cl)
        add(fat.fat2[current], 90.0f);

    /* Priority 2: Sequential assumption (80) */
    add(current + 1, 80.0f);

    /* Priority 3: Bit-flip corrections of corrupt entry (70) */
    if (fat.status[current] == FAT_CORRUPT) {
        uint32_t flips[28];
        int nflips = fat_bitflip_candidates(fat.merged[current], max_cl, flips, 28);
        for (int i = 0; i < nflips; i++) {
            float dist_penalty = (float)std::abs((int64_t)flips[i] - (int64_t)current) / 1000.0f;
            add(flips[i], 70.0f - std::min(dist_penalty, 20.0f));
        }
    }

    /* Priority 4: Sequential from previous (reinforcement) (60) */
    /* If previous step was sequential, next is likely sequential too */
    /* Already covered by priority 2, but could add current+2 etc. */

    /* Priority 5: Nearby unclaimed JPEG clusters (50 - distance) */
    int radius = ctx.search_radius;
    for (int offset = 2; offset <= radius; offset++) {
        for (int sign = -1; sign <= 1; sign += 2) {
            uint32_t cl = current + sign * offset;
            if (cl < 2 || cl > max_cl) continue;
            uint32_t idx = cl - 2;
            if (idx >= ctx.cluster_map.size()) continue;
            if (cmap[idx].content_type != CTYPE_JPEG_SCAN &&
                cmap[idx].content_type != CTYPE_JPEG_HEADER) continue;
            /* Skip if claimed by a high-scoring chain (allow stealing from low scores) */
            if (ctx.claimed_score.size() > idx && ctx.claimed_score[idx] > 0.7f)
                continue;
            add(cl, 50.0f - (float)offset * 0.5f);
        }
    }

    /* Demote FAT-pointed candidates that target cross-linked clusters.
     * A cross-linked cluster (refcount > 1) is claimed by multiple chains,
     * so following it is risky. Drop priority below sequential (80). */
    for (auto &c : cands) {
        if (c.priority >= 90.0f && c.cluster >= 2 &&
            c.cluster < (uint32_t)ctx.cluster_refcount.size() &&
            ctx.cluster_refcount[c.cluster] > 1) {
            log_debug(ctx, "  candidate %u: cross-linked (refcount %d), "
                      "priority %.0f->70",
                      c.cluster, ctx.cluster_refcount[c.cluster], c.priority);
            c.priority = 70.0f;
        }
    }

    /* Sort by priority descending */
    std::sort(cands.begin(), cands.end(),
              [](const Candidate &a, const Candidate &b) { return a.priority > b.priority; });

    /* ff00 pre-filter: reject candidates that are clearly not JPEG entropy.
     * Within-file ff00 variation commonly reaches 16-57x (tested on real
     * images), so ratio/tolerance-based filters reject valid clusters.
     * Only reject candidates with zero ff00 when current has high ff00. */
    uint32_t cur_idx = current - 2;
    uint16_t cur_ff00 = (cur_idx < cmap.size()) ? cmap[cur_idx].ff00_count : 0;

    int count = 0;
    for (auto &c : cands) {
        if (count >= max_candidates) break;

        /* ff00 density pre-filter: only reject clearly non-JPEG data */
        if (ctx.features.ff00_prefilter && chain_length > 2 &&
            cur_ff00 > 20 && c.priority < 80.0f) {
            uint32_t cidx = c.cluster - 2;
            if (cidx < cmap.size() && cmap[cidx].ff00_count == 0)
                continue;
        }

        /* Skip empty/non-JPEG/low-entropy clusters (except FAT-pointed) */
        uint32_t cidx = c.cluster - 2;
        if (cidx < cmap.size() && c.priority < 80.0f) {
            auto ct = cmap[cidx].content_type;
            if (ct == CTYPE_EMPTY || ct == CTYPE_NON_JPEG)
                continue;
            /* BFD entropy pre-filter: JPEG scan data has entropy > 7.0 */
            if (cmap[cidx].entropy < 6.5f && ct != CTYPE_JPEG_HEADER)
                continue;
        }

        out_candidates[count] = c.cluster;
        out_priorities[count] = c.priority;
        count++;
    }

    return count;
}
