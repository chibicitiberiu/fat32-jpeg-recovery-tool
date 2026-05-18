/*
 * sdrecov - SD Card JPEG Recovery Engine
 * Main entry point: CLI parsing, pipeline orchestration.
 */
#include "sdrecov.h"
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <sys/stat.h>
#include <cerrno>

static void print_usage()
{
    fprintf(stderr,
        "sdrecov %s - SD Card JPEG Recovery Engine\n\n"
        "Usage: sdrecov [options] -i <image> -o <output_dir>\n\n"
        "Required:\n"
        "  -i, --image <file>         Raw disk image\n"
        "  -o, --output <dir>         Output directory\n\n"
        "Partition:\n"
        "  -p, --partition <N>        Use MBR partition N (1-4)\n"
        "  --partition-offset <bytes> Manual byte offset\n\n"
        "Optional:\n"
        "  --image2 <file>            Second read for byte-level merge\n"
        "  --threads <N>              Recovery threads (default: 4)\n"
        "  --min-score <0-1>          Min chain score (default: 0.3)\n"
        "  --max-candidates <N>       Per-step candidate cap (default: 50)\n"
        "  --search-radius <N>        Nearby cluster radius (default: 64)\n"
        "  --bit-flips <1|2>          Max bit-flip corrections (default: 1)\n"
        "  --debug-seeds-offset <N>   Skip first N seeds (for testing)\n"
        "  --debug-seeds-limit <N>    Process at most N seeds (for testing)\n"
        "  --cluster-size <bytes>     Override cluster size (skip boot sector parsing)\n"
        "  --data-offset <bytes>      Override data area offset from partition start\n"
        "  --search <mode>            Search strategy (default: greedy):\n"
        "                               greedy - fast, picks best at each step\n"
        "                               beam   - keeps N chains alive (set N with --beam-width)\n"
        "                               full   - exhaustive DFS, tries ALL branches (very slow!)\n"
        "  --beam-width <N>           Beam search: chains to keep per step (default: 3)\n"
        "  --max-backtracks <N>       Greedy mode: backtracks per seed (default: 5)\n"
        "  -v, --verbose              Increase verbosity\n"
        "  -q, --quiet                Suppress progress\n"
        "  --dump-cluster-map <file>  Dump cluster map and exit\n"
        "  --dump-seeds <file>        Dump seed list and exit\n"
        "  --dump-fat <file>          Dump merged FAT and exit\n"
        "  -h, --help                 This help\n"
        "  --version                  Version info\n"
        "\n"
        "Feature flags:\n"
        "  --enable-all               Enable all features (default)\n"
        "  --disable-all              Disable all features (bare minimum)\n"
        "  --enable-<feature>         Enable a specific feature\n"
        "  --disable-<feature>        Disable a specific feature\n"
        "  Features: fast-path, chain-repair, sequential-scan, seam-detection,\n"
        "    chain-adequacy, size-estimation, oversize-terminate, boundary-check,\n"
        "    mcu-rate-filter, mcu-progress-score, ff00-prefilter, entropy-filter,\n"
        "    dc-bounds, rst-expectancy, dht-fallback, header-graft, annex-k-retry,\n"
        "    lenient-ff, tolerant-validate, rst-recovery, thumbnail-validate,\n"
        "    template-tables, mid-cluster-soi\n",
        SDRECOV_VERSION);
}

enum LongOpt {
    OPT_PART_OFFSET = 256, OPT_IMAGE2, OPT_THREADS, OPT_MIN_SCORE,
    OPT_MAX_CAND, OPT_RADIUS, OPT_BITFLIPS,
    OPT_DUMP_CMAP, OPT_DUMP_SEEDS, OPT_DUMP_FAT, OPT_VERSION,
    OPT_SEEDS_OFFSET, OPT_SEEDS_LIMIT, OPT_BEAM_WIDTH, OPT_MAX_BACKTRACKS,
    OPT_SEARCH, OPT_GEO_CLUSTER, OPT_GEO_DATA
};

static struct option long_options[] = {
    {"image",            required_argument, 0, 'i'},
    {"output",           required_argument, 0, 'o'},
    {"partition",        required_argument, 0, 'p'},
    {"partition-offset", required_argument, 0, OPT_PART_OFFSET},
    {"image2",           required_argument, 0, OPT_IMAGE2},
    {"threads",          required_argument, 0, OPT_THREADS},
    {"min-score",        required_argument, 0, OPT_MIN_SCORE},
    {"max-candidates",   required_argument, 0, OPT_MAX_CAND},
    {"search-radius",    required_argument, 0, OPT_RADIUS},
    {"bit-flips",        required_argument, 0, OPT_BITFLIPS},
    {"dump-cluster-map", required_argument, 0, OPT_DUMP_CMAP},
    {"dump-seeds",       required_argument, 0, OPT_DUMP_SEEDS},
    {"dump-fat",         required_argument, 0, OPT_DUMP_FAT},
    {"debug-seeds-offset", required_argument, 0, OPT_SEEDS_OFFSET},
    {"debug-seeds-limit",  required_argument, 0, OPT_SEEDS_LIMIT},
    {"beam-width",         required_argument, 0, OPT_BEAM_WIDTH},
    {"max-backtracks",     required_argument, 0, OPT_MAX_BACKTRACKS},
    {"search",             required_argument, 0, OPT_SEARCH},
    {"cluster-size",       required_argument, 0, OPT_GEO_CLUSTER},
    {"data-offset",        required_argument, 0, OPT_GEO_DATA},
    {"verbose",          no_argument,       0, 'v'},
    {"quiet",            no_argument,       0, 'q'},
    {"help",             no_argument,       0, 'h'},
    {"version",          no_argument,       0, OPT_VERSION},
    {0, 0, 0, 0}
};

int main(int argc, char **argv)
{
    RecoveryContext ctx;

    const char *image_path  = nullptr;
    const char *image2_path = nullptr;
    const char *output_path = nullptr;
    int   partition_num = 0;
    uint64_t partition_offset = 0;
    bool  offset_set = false;

    /* Pre-parse feature flags before getopt_long.
     * Collect non-feature args into a new argv for getopt. */
    std::vector<char*> new_argv;
    new_argv.push_back(argv[0]);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--enable-all") == 0) {
            ctx.features.enable_all();
        } else if (strcmp(argv[i], "--disable-all") == 0) {
            ctx.features.disable_all();
        } else if (strncmp(argv[i], "--enable-", 9) == 0) {
            const char *name = argv[i] + 9;
            bool found = false;
            for (int f = 0; FEATURE_FLAG_TABLE[f].name; f++) {
                if (strcmp(FEATURE_FLAG_TABLE[f].name, name) == 0) {
                    ctx.features.*FEATURE_FLAG_TABLE[f].ptr = true;
                    found = true; break;
                }
            }
            if (!found) { fprintf(stderr, "Unknown feature: %s\n", name); return 1; }
        } else if (strncmp(argv[i], "--disable-", 10) == 0) {
            const char *name = argv[i] + 10;
            bool found = false;
            for (int f = 0; FEATURE_FLAG_TABLE[f].name; f++) {
                if (strcmp(FEATURE_FLAG_TABLE[f].name, name) == 0) {
                    ctx.features.*FEATURE_FLAG_TABLE[f].ptr = false;
                    found = true; break;
                }
            }
            if (!found) { fprintf(stderr, "Unknown feature: %s\n", name); return 1; }
        } else {
            new_argv.push_back(argv[i]);
        }
    }
    argc = (int)new_argv.size();
    argv = new_argv.data();

    int c;
    while ((c = getopt_long(argc, argv, "i:o:p:vqh", long_options, nullptr)) != -1) {
        switch (c) {
        case 'i': image_path = optarg; break;
        case 'o': output_path = optarg; break;
        case 'p': partition_num = atoi(optarg); break;
        case OPT_PART_OFFSET: partition_offset = strtoull(optarg, nullptr, 0); offset_set = true; break;
        case OPT_IMAGE2:      image2_path = optarg; break;
        case OPT_THREADS:     ctx.threads = atoi(optarg); break;
        case OPT_MIN_SCORE:   ctx.min_score = strtof(optarg, nullptr); break;
        case OPT_MAX_CAND:    ctx.max_candidates = atoi(optarg); break;
        case OPT_RADIUS:      ctx.search_radius = atoi(optarg); break;
        case OPT_BITFLIPS:    ctx.max_bit_flips = atoi(optarg); break;
        case OPT_SEEDS_OFFSET:    ctx.seeds_offset = atoi(optarg); break;
        case OPT_SEEDS_LIMIT:     ctx.seeds_limit = atoi(optarg); break;
        case OPT_BEAM_WIDTH:      ctx.beam_width = atoi(optarg); break;
        case OPT_MAX_BACKTRACKS:  ctx.max_backtracks = atoi(optarg); break;
        case OPT_GEO_CLUSTER: ctx.geo_cluster_size = atoi(optarg); break;
        case OPT_GEO_DATA:    ctx.geo_data_offset = strtoull(optarg, nullptr, 0); break;
        case OPT_SEARCH:
            if (strcmp(optarg, "greedy") == 0)    ctx.search_mode = RecoveryContext::SEARCH_GREEDY;
            else if (strcmp(optarg, "beam") == 0) ctx.search_mode = RecoveryContext::SEARCH_BEAM;
            else if (strcmp(optarg, "full") == 0) ctx.search_mode = RecoveryContext::SEARCH_FULL;
            else { log_error("--search must be greedy, beam, or full"); return 1; }
            break;
        case 'v': ctx.verbosity++; break;
        case 'q': ctx.verbosity = 0; break;
        case 'h': print_usage(); return 0;
        case OPT_VERSION: fprintf(stderr, "sdrecov %s\n", SDRECOV_VERSION); return 0;
        default: print_usage(); return 1;
        }
    }

    if (!image_path) {
        log_error("--image is required");
        return 1;
    }
    if (!output_path) {
        log_error("--output is required");
        return 1;
    }

    ctx.output_dir = output_path;
    mkdir(ctx.output_dir.c_str(), 0755);
    mkdir((ctx.output_dir + "/files").c_str(), 0755);
    log_init(ctx);
    log_progress(ctx, "sdrecov %s starting", SDRECOV_VERSION);

    /* ---- Stage 1: Load and analyze ---- */
    log_progress(ctx, "[stage 1/3] Loading disk image...");

    if (!ctx.disk.primary.open(image_path))
        return 1;
    if (image2_path && !ctx.disk.secondary.open(image2_path))
        return 1;

    log_progress(ctx, "  Image: %.1f GB (%zu bytes)",
                 ctx.disk.size() / (1024.0 * 1024.0 * 1024.0), ctx.disk.size());

    /* Partition offset */
    if (!offset_set) {
        partition_offset = partition_get_offset(ctx.disk.data(), ctx.disk.size(), partition_num);
        if (partition_offset == UINT64_MAX) return 1;
    }
    log_progress(ctx, "  Partition offset: %llu bytes (sector %llu)",
                 (unsigned long long)partition_offset,
                 (unsigned long long)(partition_offset / SECTOR_SIZE));

    /* Parse FAT32 - or use manual geometry if boot sector is destroyed */
    if (ctx.geo_cluster_size > 0 && ctx.geo_data_offset > 0) {
        auto &g = ctx.disk.geo;
        g.bytes_per_cluster = ctx.geo_cluster_size;
        g.bytes_per_sector = 512;
        g.sectors_per_cluster = ctx.geo_cluster_size / 512;
        g.data_offset = partition_offset + ctx.geo_data_offset;
        g.partition_offset = partition_offset;
        uint64_t data_size = ctx.disk.size() - g.data_offset;
        g.total_clusters = data_size / g.bytes_per_cluster;
        g.root_cluster = 2;
        g.total_sectors = ctx.disk.size() / 512;

        /* Try to find FAT tables even with manual geometry */
        auto detected = fat32_autodetect(ctx.disk.data(), ctx.disk.size(), partition_offset);
        if (detected.valid && detected.geo.fat1_offset > 0) {
            g.fat1_offset = detected.geo.fat1_offset;
            g.fat2_offset = detected.geo.fat2_offset;
            g.sectors_per_fat = detected.geo.sectors_per_fat;
            g.num_fats = detected.geo.num_fats;
            g.reserved_sectors = (g.fat1_offset - partition_offset) / 512;
            log_progress(ctx, "  Manual geometry + autodetected FATs at %llu, %llu (confidence %.2f)",
                         (unsigned long long)g.fat1_offset, (unsigned long long)g.fat2_offset,
                         detected.confidence);
        } else {
            g.fat1_offset = 0;
            g.fat2_offset = 0;
            g.sectors_per_fat = 0;
            g.num_fats = 0;
            g.reserved_sectors = ctx.geo_data_offset / 512;
            log_progress(ctx, "  Manual geometry, no FAT tables found");
        }
        log_progress(ctx, "  Using manual geometry: %u clusters, %u bytes/cluster, data at %llu",
                     g.total_clusters, g.bytes_per_cluster, (unsigned long long)g.data_offset);
    } else if (!fat32_parse(ctx.disk, partition_offset)) {
        log_error("cannot parse FAT32 (tried boot sector, backup, and autodetection)");
        log_error("hint: use --cluster-size and --data-offset to specify geometry manually");
        return 1;
    }
    auto &g = ctx.disk.geo;
    log_progress(ctx, "  FAT32: %u clusters, %u bytes/cluster, %u sectors/FAT",
                 g.total_clusters, g.bytes_per_cluster, g.sectors_per_fat);

    /* Read and merge FATs */
    fat32_read_tables(ctx.disk, ctx.fat);
    fat_merge(ctx.fat, g.total_clusters);

    uint32_t fat_stats[6] = {};
    for (uint32_t i = 2; i < ctx.fat.count(); i++)
        fat_stats[ctx.fat.status[i]]++;
    log_progress(ctx, "  FAT merged: %u valid, %u free, %u eof, %u corrupt, %u bad",
                 fat_stats[FAT_VALID], fat_stats[FAT_FREE], fat_stats[FAT_EOF],
                 fat_stats[FAT_CORRUPT], fat_stats[FAT_BAD]);

    /* Cross-link detection: count clusters pointed to by multiple chains */
    fat_build_refcount(ctx.fat, g.total_clusters, ctx.cluster_refcount);
    uint32_t cross_linked = 0;
    for (auto r : ctx.cluster_refcount) if (r > 1) cross_linked++;
    log_progress(ctx, "  Cross-linked clusters: %u", cross_linked);

    /* FAT confidence: fraction of allocated entries that are valid */
    uint32_t total_allocated = fat_stats[FAT_VALID] + fat_stats[FAT_EOF] + fat_stats[FAT_CORRUPT];
    if (total_allocated > 0)
        ctx.fat_confidence = (float)(fat_stats[FAT_VALID] + fat_stats[FAT_EOF]) / total_allocated;
    log_progress(ctx, "  FAT confidence: %.1f%%", ctx.fat_confidence * 100);

    /* Build cluster map */
    log_progress(ctx, "[stage 1/3] Classifying %u clusters...", g.total_clusters);
    cluster_map_build(ctx.disk, ctx.cluster_map);

    uint32_t ct[8] = {};
    for (auto &cf : ctx.cluster_map) ct[cf.content_type]++;
    log_progress(ctx, "  Clusters: %u JPEG_HEADER, %u JPEG_SCAN, %u NON_JPEG, "
                 "%u EMPTY, %u BAD, %u UNKNOWN",
                 ct[CTYPE_JPEG_HEADER], ct[CTYPE_JPEG_SCAN], ct[CTYPE_NON_JPEG],
                 ct[CTYPE_EMPTY], ct[CTYPE_BAD_SECTOR], ct[CTYPE_UNKNOWN]);

    /* Build seeds */
    log_progress(ctx, "[stage 1/3] Building seed list...");
    seed_build(ctx);
    log_progress(ctx, "  Seeds: %d", (int)ctx.seeds.size());

    /* Build template library from intact headers */
    log_progress(ctx, "[stage 1/3] Building template library...");
    template_library_build(ctx);
    log_progress(ctx, "  Templates: %d", (int)ctx.templates.size());

    /* ---- Diagnostic dump modes ---- */
    /* TODO: implement --dump-cluster-map, --dump-seeds, --dump-fat as JSON output */

    /* ---- Stage 2: Recover files ---- */
    const char *mode_names[] = {"greedy", "beam", "full"};
    log_progress(ctx, "[stage 2/3] Starting recovery engine (%d seeds, %d threads, search=%s%s)...",
                 (int)ctx.seeds.size(), ctx.threads,
                 mode_names[ctx.search_mode],
                 ctx.search_mode == RecoveryContext::SEARCH_FULL ? " WARNING: very slow!" : "");

    /* Propagate feature flags to globals used by standalone parsers */
    extern bool g_dht_fallback_enabled;
    extern bool g_lenient_ff_enabled;
    g_dht_fallback_enabled = ctx.features.dht_fallback;
    g_lenient_ff_enabled = ctx.features.lenient_ff;

    int recovered = engine_recover(ctx);

    log_progress(ctx, "[stage 2/3] Recovery complete: %d files recovered", recovered);

    /* TODO: Stage 3 (post-processing: compositing, report) */

    /* Write JSON report */
    {
        auto report_path = ctx.output_dir + "/report.json";
        FILE *rf = fopen(report_path.c_str(), "w");
        if (rf) {
            fprintf(rf, "{\n  \"version\": \"%s\",\n", SDRECOV_VERSION);
            fprintf(rf, "  \"image\": \"%s\",\n", image_path);
            fprintf(rf, "  \"image_size\": %zu,\n", ctx.disk.size());
            fprintf(rf, "  \"partition_offset\": %llu,\n", (unsigned long long)partition_offset);
            fprintf(rf, "  \"total_clusters\": %u,\n", g.total_clusters);
            fprintf(rf, "  \"bytes_per_cluster\": %u,\n", g.bytes_per_cluster);
            fprintf(rf, "  \"fat_valid\": %u,\n", fat_stats[FAT_VALID]);
            fprintf(rf, "  \"fat_corrupt\": %u,\n", fat_stats[FAT_CORRUPT]);
            fprintf(rf, "  \"seeds_total\": %d,\n", (int)ctx.seeds.size());
            fprintf(rf, "  \"templates\": %d,\n", (int)ctx.templates.size());
            fprintf(rf, "  \"files_recovered\": %d,\n", recovered);
            fprintf(rf, "  \"cluster_types\": {\n");
            fprintf(rf, "    \"jpeg_header\": %u,\n", ct[CTYPE_JPEG_HEADER]);
            fprintf(rf, "    \"jpeg_scan\": %u,\n", ct[CTYPE_JPEG_SCAN]);
            fprintf(rf, "    \"non_jpeg\": %u,\n", ct[CTYPE_NON_JPEG]);
            fprintf(rf, "    \"empty\": %u,\n", ct[CTYPE_EMPTY]);
            fprintf(rf, "    \"bad_sector\": %u,\n", ct[CTYPE_BAD_SECTOR]);
            fprintf(rf, "    \"unknown\": %u\n", ct[CTYPE_UNKNOWN]);
            fprintf(rf, "  }\n}\n");
            fclose(rf);
        }
    }

    /* Write summary */
    {
        auto summary_path = ctx.output_dir + "/summary.txt";
        FILE *sf = fopen(summary_path.c_str(), "w");
        if (sf) {
            fprintf(sf, "sdrecov %s recovery summary\n\n", SDRECOV_VERSION);
            fprintf(sf, "Image: %s (%.1f GB)\n", image_path,
                    ctx.disk.size() / (1024.0 * 1024.0 * 1024.0));
            fprintf(sf, "Partition offset: %llu bytes\n", (unsigned long long)partition_offset);
            fprintf(sf, "FAT32: %u clusters, %u bytes/cluster\n",
                    g.total_clusters, g.bytes_per_cluster);
            fprintf(sf, "FAT merged: %u valid, %u corrupt\n",
                    fat_stats[FAT_VALID], fat_stats[FAT_CORRUPT]);
            fprintf(sf, "Cluster map: %u JPEG_HEADER, %u JPEG_SCAN, %u EMPTY, %u UNKNOWN\n",
                    ct[CTYPE_JPEG_HEADER], ct[CTYPE_JPEG_SCAN], ct[CTYPE_EMPTY], ct[CTYPE_UNKNOWN]);
            fprintf(sf, "Seeds: %d\n", (int)ctx.seeds.size());
            fprintf(sf, "Templates: %d\n", (int)ctx.templates.size());
            int thumb_count = 0;
            for (auto &s : ctx.seeds) if (s.has_thumbnail) thumb_count++;
            fprintf(sf, "Thumbnails: %d\n", thumb_count);
            fprintf(sf, "Files recovered: %d\n", recovered);
            fclose(sf);
        }
    }

    log_progress(ctx, "Done. %d files in %s/files/", recovered, ctx.output_dir.c_str());
    return 0;
}
