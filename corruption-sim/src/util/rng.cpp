/*
 * rng.cpp - Deterministic PRNG with convenience wrappers.
 * All randomness flows through SimContext::rng for reproducibility.
 */
#include "corrsim.h"
#include <algorithm>

void rng_init(SimContext &ctx)
{
    if (ctx.cfg.rng_seed == 0) {
        std::random_device rd;
        ctx.cfg.rng_seed = rd();
    }
    ctx.rng.seed(ctx.cfg.rng_seed);
}

double rng_uniform(SimContext &ctx, double lo, double hi)
{
    std::uniform_real_distribution<double> dist(lo, hi);
    return dist(ctx.rng);
}

int rng_int(SimContext &ctx, int lo, int hi)
{
    std::uniform_int_distribution<int> dist(lo, hi);
    return dist(ctx.rng);
}

bool rng_bernoulli(SimContext &ctx, double p)
{
    std::uniform_real_distribution<double> dist(0.0, 1.0);
    return dist(ctx.rng) < p;
}

uint64_t rng_geometric(SimContext &ctx, double p)
{
    if (p <= 0.0) return UINT64_MAX;
    if (p >= 1.0) return 0;
    std::geometric_distribution<uint64_t> dist(p);
    return dist(ctx.rng);
}

std::string rng_pick(SimContext &ctx, const std::vector<std::string> &v)
{
    if (v.empty()) return "";
    return v[rng_int(ctx, 0, (int)v.size() - 1)];
}

void rng_shuffle(SimContext &ctx, std::vector<std::string> &v)
{
    std::shuffle(v.begin(), v.end(), ctx.rng);
}

void rng_shuffle_u32(SimContext &ctx, std::vector<uint32_t> &v)
{
    std::shuffle(v.begin(), v.end(), ctx.rng);
}
