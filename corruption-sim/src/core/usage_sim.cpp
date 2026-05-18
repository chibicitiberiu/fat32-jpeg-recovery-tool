/*
 * usage_sim.cpp - Stage 3: Simulate user file operations to create
 * realistic fragmentation and deleted file remnants.
 */
#include "corrsim.h"
#include <algorithm>
#include <sstream>

/* List all files currently in the image using mdir */
static bool refresh_file_list(SimContext &ctx)
{
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "mdir -/ -b -i '%s' '::'",
             ctx.cfg.image_path.c_str());
    auto r = run_cmd(cmd);
    if (r.exit_code != 0) {
        log_error("mdir failed: %s", r.out.c_str());
        return false;
    }

    ctx.image_files.clear();
    std::istringstream ss(r.out);
    std::string line;
    while (std::getline(ss, line)) {
        // mdir -b outputs lines like "::/DCIM/100CANON/DSC_0001.JPG"
        if (line.size() > 3 && line.substr(0, 3) == "::/") {
            std::string path = line.substr(3);
            // Skip directory entries (end with /)
            if (!path.empty() && path.back() != '/')
                ctx.image_files.push_back(path);
        }
    }
    return true;
}

/* Generate a random destination path for a new file */
static std::string random_dest_path(SimContext &ctx)
{
    // Pick from existing directories or root
    std::vector<std::string> dirs;
    dirs.push_back("");  // root

    for (auto &f : ctx.image_files) {
        auto slash = f.rfind('/');
        if (slash != std::string::npos) {
            std::string dir = f.substr(0, slash);
            dirs.push_back(dir);
        }
    }
    // Deduplicate
    std::sort(dirs.begin(), dirs.end());
    dirs.erase(std::unique(dirs.begin(), dirs.end()), dirs.end());

    std::string dir = dirs[rng_int(ctx, 0, (int)dirs.size() - 1)];

    // Generate a simple filename
    char buf[64];
    snprintf(buf, sizeof(buf), "COPY_%04d.JPG", rng_int(ctx, 1, 9999));

    if (dir.empty()) return buf;
    return dir + "/" + buf;
}

bool usage_simulate(SimContext &ctx)
{
    if (ctx.cfg.skip_sim) {
        log_progress(ctx, "Stage 3: Usage simulation skipped");
        return true;
    }

    log_progress(ctx, "Stage 3: Simulating %d user operations",
                 ctx.cfg.usage.operations);

    if (!refresh_file_list(ctx))
        return false;

    int deletes = 0, copies = 0, moves = 0;

    for (int op = 0; op < ctx.cfg.usage.operations; op++) {
        if (ctx.image_files.empty()) {
            log_info(ctx, "No files left in image, stopping simulation");
            break;
        }

        double roll = rng_uniform(ctx, 0.0, 1.0);

        if (roll < ctx.cfg.usage.delete_prob && ctx.image_files.size() > 5) {
            // DELETE a random file
            int idx = rng_int(ctx, 0, (int)ctx.image_files.size() - 1);
            std::string target = ctx.image_files[idx];

            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "mdel -D o -i '%s' '::/%s'",
                     ctx.cfg.image_path.c_str(), target.c_str());
            auto r = run_cmd(cmd);
            if (r.exit_code == 0) {
                log_detail(ctx, "op %d: DELETE /%s", op + 1, target.c_str());
                ctx.sim_ops.push_back({"delete", target, "", "", op + 1});
                ctx.image_files.erase(ctx.image_files.begin() + idx);
                deletes++;
            }

        } else if (roll < ctx.cfg.usage.delete_prob + ctx.cfg.usage.move_prob) {
            // MOVE a random file (copy + delete original)
            int idx = rng_int(ctx, 0, (int)ctx.image_files.size() - 1);
            std::string src = ctx.image_files[idx];
            std::string dest = random_dest_path(ctx);

            // Avoid moving to same path
            if (dest == src) continue;

            char cmd[4096];
            // mcopy from image to image isn't supported, so we extract + reinsert
            // Instead, use mmove which handles this natively
            snprintf(cmd, sizeof(cmd), "mmove -D o -i '%s' '::/%s' '::/%s'",
                     ctx.cfg.image_path.c_str(), src.c_str(), dest.c_str());
            auto r = run_cmd(cmd);
            if (r.exit_code == 0) {
                log_detail(ctx, "op %d: MOVE /%s -> /%s", op + 1,
                           src.c_str(), dest.c_str());
                ctx.sim_ops.push_back({"move", src, dest, "", op + 1});
                ctx.image_files[idx] = dest;
                moves++;
            }

        } else {
            // COPY a new file from source into image
            if (ctx.source_files.empty()) continue;
            std::string src = ctx.source_files[rng_int(ctx, 0,
                (int)ctx.source_files.size() - 1)];
            std::string dest = random_dest_path(ctx);

            char cmd[4096];
            snprintf(cmd, sizeof(cmd), "mcopy -D o -n -i '%s' '%s' '::/%s'",
                     ctx.cfg.image_path.c_str(), src.c_str(), dest.c_str());
            auto r = run_cmd(cmd);
            if (r.exit_code == 0) {
                log_detail(ctx, "op %d: COPY %s -> /%s", op + 1,
                           src.c_str(), dest.c_str());
                ctx.sim_ops.push_back({"copy", dest, "", src, op + 1});
                ctx.placed_files.push_back({dest, src});
                ctx.image_files.push_back(dest);
                copies++;
            }
        }
    }

    log_progress(ctx, "  Simulated %d deletes, %d copies, %d moves",
                 deletes, copies, moves);
    return true;
}
