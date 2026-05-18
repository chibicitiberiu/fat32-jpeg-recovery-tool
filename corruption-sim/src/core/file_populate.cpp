/*
 * file_populate.cpp - Stage 2: Populate FAT32 image with files in
 * realistic folder structures using mtools.
 */
#include "corrsim.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstdio>
#include <set>

/* Scan source directory for JPEG files */
static bool scan_source_files(SimContext &ctx)
{
    DIR *d = opendir(ctx.cfg.populate.source_dir.c_str());
    if (!d) {
        log_error("cannot open source directory: %s",
                  ctx.cfg.populate.source_dir.c_str());
        return false;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_type != DT_REG && ent->d_type != DT_UNKNOWN)
            continue;
        std::string name = ent->d_name;
        // Accept common image extensions
        auto ext_pos = name.rfind('.');
        if (ext_pos == std::string::npos) continue;
        std::string ext = name.substr(ext_pos);
        for (auto &c : ext) c = tolower(c);
        if (ext == ".jpg" || ext == ".jpeg" || ext == ".png" || ext == ".bmp")
            ctx.source_files.push_back(ctx.cfg.populate.source_dir + "/" + name);
    }
    closedir(d);

    if (ctx.source_files.empty()) {
        log_error("no image files found in %s", ctx.cfg.populate.source_dir.c_str());
        return false;
    }

    std::sort(ctx.source_files.begin(), ctx.source_files.end());
    log_info(ctx, "Found %zu source files in %s",
             ctx.source_files.size(), ctx.cfg.populate.source_dir.c_str());
    return true;
}

/* Generate a camera-style filename: DSC_0001.JPG, IMG_20240315_143022.jpg etc. */
static std::string gen_camera_name(SimContext &ctx, int seq)
{
    auto &prefixes = file_name_prefixes();
    std::string prefix = rng_pick(ctx, prefixes);

    if (prefix == "IMG_") {
        // Generate date-style: IMG_YYYYMMDD_HHMMSS.jpg
        int year  = rng_int(ctx, 2019, 2025);
        int month = rng_int(ctx, 1, 12);
        int day   = rng_int(ctx, 1, 28);
        int hour  = rng_int(ctx, 6, 22);
        int min   = rng_int(ctx, 0, 59);
        int sec   = rng_int(ctx, 0, 59);
        char buf[64];
        snprintf(buf, sizeof(buf), "IMG_%04d%02d%02d_%02d%02d%02d.jpg",
                 year, month, day, hour, min, sec);
        return buf;
    }
    // Numbered style: DSC_0042.JPG
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%04d.JPG", prefix.c_str(), seq);
    return buf;
}

/* Generate a descriptive filename */
static std::string gen_descriptive_name(SimContext &ctx)
{
    auto &names = file_name_descriptive();
    std::string name = rng_pick(ctx, names);
    // Random extension case
    if (rng_bernoulli(ctx, 0.5))
        return name + ".jpg";
    return name + ".JPG";
}

/* Create a directory in the image, handling nested paths */
static bool mmd_path(const SimContext &ctx, const std::string &path)
{
    // mmd needs each component created separately
    std::string accum;
    size_t pos = 0;
    while (pos < path.size()) {
        size_t slash = path.find('/', pos);
        if (slash == std::string::npos) slash = path.size();
        std::string component = path.substr(pos, slash - pos);
        if (!component.empty()) {
            if (!accum.empty()) accum += "/";
            accum += component;
            char cmd[2048];
            snprintf(cmd, sizeof(cmd), "mmd -D s -i '%s' '::/%s' 2>/dev/null",
                     ctx.cfg.image_path.c_str(), accum.c_str());
            run_cmd(cmd);  // ignore error if already exists
        }
        pos = slash + 1;
    }
    return true;
}

/* Copy a file into the image */
static bool mcopy_file(const SimContext &ctx, const std::string &src,
                       const std::string &dest_path)
{
    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "mcopy -D o -n -i '%s' '%s' '::/%s'",
             ctx.cfg.image_path.c_str(), src.c_str(), dest_path.c_str());
    auto r = run_cmd(cmd);
    return r.exit_code == 0;
}

bool file_populate(SimContext &ctx)
{
    log_progress(ctx, "Stage 2: Populating image with files");

    /* Prevent mtools from prompting on /dev/tty for confirmations */
    setenv("MTOOLS_SKIP_CHECK", "1", 1);
    setenv("MTOOLS_NO_VFAT", "1", 1);

    if (!scan_source_files(ctx))
        return false;

    int file_count = ctx.cfg.populate.file_count;
    if (file_count > (int)ctx.source_files.size())
        file_count = (int)ctx.source_files.size();

    // Shuffle source files and pick the first file_count
    rng_shuffle(ctx, ctx.source_files);

    // Build folder structure
    struct FolderPlan {
        std::string path;
        int file_target;  // approximate files to place here
    };
    std::vector<FolderPlan> folders;

    // DCIM camera folder (30-50% of files)
    if (ctx.cfg.populate.dcim) {
        auto &cam_names = folder_names_camera();
        // Pick a brand subfolder
        std::string brand;
        for (auto &n : cam_names) {
            if (n.substr(0, 3) == "100") {
                brand = n;
                break;
            }
        }
        if (brand.empty()) brand = "100CANON";
        std::string dcim_path = "DCIM/" + brand;
        int dcim_count = rng_int(ctx, file_count * 30 / 100, file_count * 50 / 100);
        folders.push_back({dcim_path, dcim_count});
        log_detail(ctx, "DCIM folder: %s (%d files)", dcim_path.c_str(), dcim_count);
    }

    // User folders (English + Romanian)
    int num_user_folders = rng_int(ctx, 3, 8);
    auto en_names = folder_names_english();
    auto ro_names = folder_names_romanian();
    rng_shuffle(ctx, en_names);
    rng_shuffle(ctx, ro_names);

    int en_idx = 0, ro_idx = 0;
    for (int i = 0; i < num_user_folders; i++) {
        std::string folder;
        bool use_romanian = ctx.cfg.populate.romanian_names &&
                            rng_bernoulli(ctx, 0.3);

        if (use_romanian && ro_idx < (int)ro_names.size()) {
            folder = ro_names[ro_idx++];
        } else if (en_idx < (int)en_names.size()) {
            folder = en_names[en_idx++];
        } else {
            continue;
        }

        // Sometimes nest deeper
        int depth = rng_int(ctx, 1, ctx.cfg.populate.max_depth);
        if (depth > 1 && !folders.empty()) {
            // Nest under an existing folder sometimes
            if (rng_bernoulli(ctx, 0.4)) {
                auto &parent = folders[rng_int(ctx, 0, (int)folders.size() - 1)];
                // Don't nest too deep
                int slashes = 0;
                for (char c : parent.path) if (c == '/') slashes++;
                if (slashes < ctx.cfg.populate.max_depth - 1) {
                    folder = parent.path + "/" + folder;
                }
            }
        }

        int target = rng_int(ctx, 2, file_count / num_user_folders + 5);
        folders.push_back({folder, target});
        log_detail(ctx, "User folder: %s (%d files)", folder.c_str(), target);
    }

    // Root directory gets some files too
    int root_count = rng_int(ctx, 1, std::max(2, file_count / 10));
    folders.push_back({"", root_count});

    // Create all directories
    std::set<std::string> created_dirs;
    for (auto &fp : folders) {
        if (!fp.path.empty() && created_dirs.find(fp.path) == created_dirs.end()) {
            mmd_path(ctx, fp.path);
            created_dirs.insert(fp.path);
            log_detail(ctx, "Created directory: /%s", fp.path.c_str());
        }
    }

    // Distribute files across folders
    int placed = 0;
    int src_idx = 0;
    int seq = 1;  // for camera-style numbering

    for (auto &fp : folders) {
        if (placed >= file_count) break;

        int count = std::min(fp.file_target, file_count - placed);
        for (int i = 0; i < count && src_idx < (int)ctx.source_files.size(); i++) {
            // Generate a filename
            std::string filename;
            bool is_dcim = fp.path.find("DCIM") != std::string::npos ||
                           fp.path.find("100") != std::string::npos;

            if (is_dcim || rng_bernoulli(ctx, 0.5)) {
                filename = gen_camera_name(ctx, seq++);
            } else {
                filename = gen_descriptive_name(ctx);
            }

            std::string dest = fp.path.empty() ? filename : fp.path + "/" + filename;
            std::string src = ctx.source_files[src_idx];

            if (mcopy_file(ctx, src, dest)) {
                ctx.image_files.push_back(dest);
                ctx.placed_files.push_back({dest, src});
                log_debug(ctx, "Placed: %s -> /%s", src.c_str(), dest.c_str());
                placed++;
                src_idx++;
            } else {
                log_detail(ctx, "Failed to copy %s to /%s", src.c_str(), dest.c_str());
                src_idx++;  // skip this source file
            }
        }
    }

    log_progress(ctx, "  Placed %d files in %zu folders", placed, folders.size());
    return placed > 0;
}
