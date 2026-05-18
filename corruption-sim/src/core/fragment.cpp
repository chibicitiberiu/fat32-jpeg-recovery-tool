/*
 * fragment.cpp - Pre-populate image with filler text files and delete
 * a fraction to create free-space gaps, forcing subsequent JPEG
 * writes to fragment across non-contiguous clusters.
 */
#include "corrsim.h"
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>
#include <algorithm>

bool fragment_fill(SimContext &ctx)
{
    auto &cfg = ctx.cfg.fragment;
    if (!cfg.enabled) return true;

    log_progress(ctx, "Stage 1b: Forced fragmentation (%d files, %.0f%% delete)",
                 cfg.file_count, cfg.delete_frac * 100);

    setenv("MTOOLS_SKIP_CHECK", "1", 1);
    setenv("MTOOLS_NO_VFAT", "1", 1);

    // Create _FRAG directory in image
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mmd -D s -i '%s' '::/_FRAG'",
             ctx.cfg.image_path.c_str());
    run_cmd(cmd);

    // Safety check: cap total filler bytes at 25% of image capacity
    uint64_t max_filler_bytes = ctx.cfg.image_size_mb * 1048576ULL / 4;
    uint64_t avg_size = ((uint64_t)cfg.min_size + cfg.max_size) / 2;
    int safe_count = cfg.file_count;
    if (avg_size > 0 && (uint64_t)cfg.file_count * avg_size > max_filler_bytes)
        safe_count = (int)(max_filler_bytes / avg_size);

    if (safe_count < cfg.file_count)
        log_info(ctx, "Capped filler count from %d to %d (25%% image limit)",
                 cfg.file_count, safe_count);

    // Create temp directory on host
    char tmpdir[] = "/tmp/corrsim_frag_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        log_error("cannot create temp directory for fragmentation");
        return false;
    }

    // Generate and copy filler text files
    std::vector<std::string> filler_names;

    for (int i = 0; i < safe_count; i++) {
        uint32_t file_size = (uint32_t)rng_int(ctx, (int)cfg.min_size,
                                                     (int)cfg.max_size);
        char fname[64];
        snprintf(fname, sizeof(fname), "FRAG%04d.TXT", i);

        // Create temp file with random ASCII content
        char tmppath[512];
        snprintf(tmppath, sizeof(tmppath), "%s/%s", tmpdir, fname);
        FILE *f = fopen(tmppath, "w");
        if (!f) continue;
        for (uint32_t b = 0; b < file_size; b++)
            fputc('A' + rng_int(ctx, 0, 25), f);
        fclose(f);

        // Copy into image
        snprintf(cmd, sizeof(cmd), "mcopy -D o -n -i '%s' '%s' '::/_FRAG/%s'",
                 ctx.cfg.image_path.c_str(), tmppath, fname);
        auto r = run_cmd(cmd);
        if (r.exit_code == 0)
            filler_names.push_back(fname);

        unlink(tmppath);

        if ((i + 1) % 100 == 0)
            log_info(ctx, "  Placed %d / %d filler files", i + 1, safe_count);
    }

    rmdir(tmpdir);

    log_info(ctx, "Placed %zu filler files in _FRAG/", filler_names.size());

    if (filler_names.empty()) return true;

    // Delete a fraction to create gaps
    rng_shuffle(ctx, filler_names);
    int delete_count = (int)(filler_names.size() * cfg.delete_frac);
    int deleted = 0;

    for (int i = 0; i < delete_count && i < (int)filler_names.size(); i++) {
        snprintf(cmd, sizeof(cmd), "mdel -i '%s' '::/_FRAG/%s'",
                 ctx.cfg.image_path.c_str(), filler_names[i].c_str());
        auto r = run_cmd(cmd);
        if (r.exit_code == 0) deleted++;
    }

    log_progress(ctx, "  Fragmentation: placed %zu, deleted %d (%.0f%% gap ratio)",
                 filler_names.size(), deleted,
                 100.0 * deleted / std::max(1, (int)filler_names.size()));

    return true;
}
