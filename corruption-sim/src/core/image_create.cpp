/*
 * image_create.cpp - Stage 1: Create blank FAT32 image using mkfs.fat.
 */
#include "corrsim.h"
#include <cstdio>
#include <unistd.h>

bool image_create(SimContext &ctx)
{
    log_progress(ctx, "Stage 1: Creating %luMB FAT32 image: %s",
                 (unsigned long)ctx.cfg.image_size_mb,
                 ctx.cfg.image_path.c_str());

    // Remove existing image if present (mkfs.fat -C refuses to overwrite)
    unlink(ctx.cfg.image_path.c_str());

    // mkfs.fat -C creates the file and formats in one step.
    // Size is in 1KB blocks.
    uint64_t size_kb = ctx.cfg.image_size_mb * 1024;

    char cmd[1024];
    snprintf(cmd, sizeof(cmd),
             "mkfs.fat -C -F 32 -s %u -n '%.11s' '%s' %lu",
             ctx.cfg.sectors_per_cluster,
             ctx.cfg.volume_label.c_str(),
             ctx.cfg.image_path.c_str(),
             (unsigned long)size_kb);

    log_detail(ctx, "Running: %s", cmd);
    auto r = run_cmd(cmd, ctx.cfg.verbosity);

    if (r.exit_code != 0) {
        log_error("mkfs.fat failed (exit %d): %s", r.exit_code, r.out.c_str());
        return false;
    }

    log_info(ctx, "Image created: %luMB, %u sectors/cluster, label '%s'",
             (unsigned long)ctx.cfg.image_size_mb,
             ctx.cfg.sectors_per_cluster,
             ctx.cfg.volume_label.c_str());
    return true;
}
