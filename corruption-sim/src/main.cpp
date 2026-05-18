/*
 * corrsim - FAT32 Corruption Simulator
 * Main entry point: CLI parsing, pipeline orchestration.
 */
#include "corrsim.h"
#include <cstdlib>
#include <cstring>
#include <getopt.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

static void print_usage()
{
    fprintf(stderr,
        "corrsim %s - FAT32 Corruption Simulator\n\n"
        "Usage: corrsim [options] -s <source_dir> -o <output.img>\n\n"
        "Required:\n"
        "  -s, --source <dir>            Source directory of JPEG files\n"
        "  -o, --output <file>           Output image path\n\n"
        "Image:\n"
        "  --size <MB>                   Image size in MB (default: 512)\n"
        "  --sectors-per-cluster <N>     Sectors per cluster (default: 8)\n"
        "  --label <name>               Volume label (default: TESTDISK)\n\n"
        "Population:\n"
        "  --file-count <N>              Files to place (default: 200)\n"
        "  --max-depth <N>               Max folder nesting (default: 4)\n"
        "  --no-romanian                 Disable Romanian folder names\n"
        "  --no-dcim                     Disable DCIM camera structure\n\n"
        "Fragmentation:\n"
        "  --fragment                    Enable forced fragmentation\n"
        "  --frag-count <N>              Filler file count (default: 500)\n"
        "  --frag-size-min <bytes>       Min filler file size (default: 2048)\n"
        "  --frag-size-max <bytes>       Max filler file size (default: 65536)\n"
        "  --frag-delete-pct <0-100>     Percent of fillers to delete (default: 60)\n\n"
        "Usage simulation:\n"
        "  --sim-ops <N>                 File operations to simulate (default: 50)\n"
        "  --no-sim                      Skip usage simulation\n\n"
        "Corruption:\n"
        "  --pre-corrupt-frac <0-1>      Pre-corruption budget fraction (default: 0)\n"
        "  --profile <name>              Preset: light, moderate, heavy, catastrophic,\n"
        "                                  metadata-only, bitrot-only, real-sd\n"
        "  --list-profiles               Show available profiles and exit\n"
        "  --passes <mask>               Bitmask: 1=bitflip, 2=ftl, 4=metadata (default: 7)\n"
        "  --ber <rate>                  Bit error rate (default: 1e-5)\n"
        "  --bias <0-1>                  0->1 flip bias (default: 0.80)\n"
        "  --degraded-frac <0-1>         Degraded erase block fraction (default: 0.10)\n"
        "  --erase-block <bytes>         Erase block size (default: 131072)\n"
        "  --ftl-block <bytes>           FTL allocation group (default: 4194304)\n"
        "  --swap-frac <0-1>             Block swap fraction (default: 0.01)\n"
        "  --zero-frac <0-1>             Block zero fraction (default: 0.005)\n"
        "  --fat-break-frac <0-1>        FAT chain break fraction (default: 0.005)\n"
        "  --dir-corrupt-frac <0-1>      Dir entry corruption fraction (default: 0.05)\n\n"
        "General:\n"
        "  --seed <N>                    RNG seed for reproducibility (default: random)\n"
        "  --manifest <file>             Ground truth output path\n"
        "  --full-manifest               Record every mutation in detail\n"
        "  -v, --verbose                 Increase verbosity (-v, -vv, -vvv)\n"
        "  -q, --quiet                   Suppress progress output\n"
        "  -h, --help                    Show help\n"
        "  --version                     Version info\n",
        CORRSIM_VERSION);
}

enum LongOpt {
    OPT_SIZE = 256, OPT_SPC, OPT_LABEL,
    OPT_FILE_COUNT, OPT_MAX_DEPTH, OPT_NO_RO, OPT_NO_DCIM,
    OPT_FRAGMENT, OPT_FRAG_COUNT, OPT_FRAG_SIZE_MIN, OPT_FRAG_SIZE_MAX,
    OPT_FRAG_DELETE_PCT,
    OPT_SIM_OPS, OPT_NO_SIM,
    OPT_PRE_CORRUPT_FRAC,
    OPT_PROFILE, OPT_LIST_PROFILES, OPT_PASSES,
    OPT_BER, OPT_BIAS, OPT_DEGRADED, OPT_ERASE_BLOCK,
    OPT_FTL_BLOCK, OPT_SWAP_FRAC, OPT_ZERO_FRAC,
    OPT_FAT_BREAK, OPT_DIR_CORRUPT,
    OPT_SEED, OPT_MANIFEST, OPT_FULL_MANIFEST, OPT_VERSION
};

static struct option long_options[] = {
    {"source",            required_argument, 0, 's'},
    {"output",            required_argument, 0, 'o'},
    {"size",              required_argument, 0, OPT_SIZE},
    {"sectors-per-cluster", required_argument, 0, OPT_SPC},
    {"label",             required_argument, 0, OPT_LABEL},
    {"file-count",        required_argument, 0, OPT_FILE_COUNT},
    {"max-depth",         required_argument, 0, OPT_MAX_DEPTH},
    {"no-romanian",       no_argument,       0, OPT_NO_RO},
    {"no-dcim",           no_argument,       0, OPT_NO_DCIM},
    {"fragment",          no_argument,       0, OPT_FRAGMENT},
    {"frag-count",        required_argument, 0, OPT_FRAG_COUNT},
    {"frag-size-min",     required_argument, 0, OPT_FRAG_SIZE_MIN},
    {"frag-size-max",     required_argument, 0, OPT_FRAG_SIZE_MAX},
    {"frag-delete-pct",   required_argument, 0, OPT_FRAG_DELETE_PCT},
    {"sim-ops",           required_argument, 0, OPT_SIM_OPS},
    {"no-sim",            no_argument,       0, OPT_NO_SIM},
    {"pre-corrupt-frac",  required_argument, 0, OPT_PRE_CORRUPT_FRAC},
    {"profile",           required_argument, 0, OPT_PROFILE},
    {"list-profiles",     no_argument,       0, OPT_LIST_PROFILES},
    {"passes",            required_argument, 0, OPT_PASSES},
    {"ber",               required_argument, 0, OPT_BER},
    {"bias",              required_argument, 0, OPT_BIAS},
    {"degraded-frac",     required_argument, 0, OPT_DEGRADED},
    {"erase-block",       required_argument, 0, OPT_ERASE_BLOCK},
    {"ftl-block",         required_argument, 0, OPT_FTL_BLOCK},
    {"swap-frac",         required_argument, 0, OPT_SWAP_FRAC},
    {"zero-frac",         required_argument, 0, OPT_ZERO_FRAC},
    {"fat-break-frac",    required_argument, 0, OPT_FAT_BREAK},
    {"dir-corrupt-frac",  required_argument, 0, OPT_DIR_CORRUPT},
    {"seed",              required_argument, 0, OPT_SEED},
    {"manifest",          required_argument, 0, OPT_MANIFEST},
    {"full-manifest",     no_argument,       0, OPT_FULL_MANIFEST},
    {"verbose",           no_argument,       0, 'v'},
    {"quiet",             no_argument,       0, 'q'},
    {"help",              no_argument,       0, 'h'},
    {"version",           no_argument,       0, OPT_VERSION},
    {0, 0, 0, 0}
};

/* Apply pre-corruption: data-region bitflip/FTL + FAT chain breaks only.
 * Boot sector, reserved sectors, FAT tables stay intact so mtools still works. */
bool pre_corruption_apply(SimContext &ctx)
{
    double frac = ctx.cfg.pre_corrupt_frac;
    if (frac <= 0.0) return true;

    log_progress(ctx, "Stage 2b: Pre-corruption (%.0f%% budget, data region + FAT chains)",
                 frac * 100);

    int fd = open(ctx.cfg.image_path.c_str(), O_RDWR);
    if (fd < 0) {
        log_error("cannot open image for pre-corruption: %s", ctx.cfg.image_path.c_str());
        return false;
    }

    struct stat st;
    fstat(fd, &st);
    size_t size = st.st_size;

    uint8_t *data = (uint8_t *)mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        log_error("mmap failed for pre-corruption");
        close(fd);
        return false;
    }

    // Parse geometry to find data_offset
    Fat32Geo geo;
    if (!fat32_parse_geo(data, size, geo)) {
        log_error("cannot parse FAT32 for pre-corruption");
        munmap(data, size);
        close(fd);
        return false;
    }
    ctx.truth.geo = geo;

    // Save original config values
    double orig_ber       = ctx.cfg.bitflip.ber;
    double orig_swap      = ctx.cfg.ftl.swap_frac;
    double orig_zero      = ctx.cfg.ftl.zero_frac;
    double orig_wrong     = ctx.cfg.ftl.wrong_data_frac;
    double orig_fat_break = ctx.cfg.metadata.fat_chain_break_frac;

    // Scale to pre-corruption budget
    ctx.cfg.bitflip.ber                  *= frac;
    ctx.cfg.ftl.swap_frac                *= frac;
    ctx.cfg.ftl.zero_frac                *= frac;
    ctx.cfg.ftl.wrong_data_frac          *= frac;
    ctx.cfg.metadata.fat_chain_break_frac *= frac;

    bool ok = true;

    if (ctx.cfg.passes & PASS_BITFLIP)
        ok = ok && corrupt_bitflip(ctx, data, size, geo.data_offset, size);

    if (ctx.cfg.passes & PASS_FTL)
        ok = ok && corrupt_ftl(ctx, data, size, geo.data_offset, size);

    if (ctx.cfg.passes & PASS_META)
        ok = ok && corrupt_fat_chains(ctx, data, size);

    // Save pre-corruption stats
    ctx.truth.pre_bits_flipped        = ctx.truth.total_bits_flipped;
    ctx.truth.pre_blocks_zeroed       = ctx.truth.blocks_zeroed;
    ctx.truth.pre_blocks_swapped      = ctx.truth.blocks_swapped;
    ctx.truth.pre_blocks_wrong_data   = ctx.truth.blocks_wrong_data;
    ctx.truth.pre_fat_entries_corrupted = ctx.truth.fat_entries_corrupted;

    // Reset counters so post-corruption accumulates separately
    ctx.truth.total_bits_flipped    = 0;
    ctx.truth.blocks_zeroed         = 0;
    ctx.truth.blocks_swapped        = 0;
    ctx.truth.blocks_wrong_data     = 0;
    ctx.truth.fat_entries_corrupted = 0;

    // Restore config to remaining budget for post-corruption
    ctx.cfg.bitflip.ber                  = orig_ber       * (1.0 - frac);
    ctx.cfg.ftl.swap_frac                = orig_swap      * (1.0 - frac);
    ctx.cfg.ftl.zero_frac                = orig_zero      * (1.0 - frac);
    ctx.cfg.ftl.wrong_data_frac          = orig_wrong     * (1.0 - frac);
    ctx.cfg.metadata.fat_chain_break_frac = orig_fat_break * (1.0 - frac);

    msync(data, size, MS_SYNC);
    munmap(data, size);
    close(fd);

    log_progress(ctx, "  Pre-corruption: %lu bits flipped, %u FAT entries broken",
                 (unsigned long)ctx.truth.pre_bits_flipped,
                 ctx.truth.pre_fat_entries_corrupted);

    return ok;
}

/* Apply corruption to the image file using mmap */
bool corruption_apply(SimContext &ctx)
{
    log_progress(ctx, "Stage 5: Applying corruption");

    int fd = open(ctx.cfg.image_path.c_str(), O_RDWR);
    if (fd < 0) {
        log_error("cannot open image for writing: %s", ctx.cfg.image_path.c_str());
        return false;
    }

    struct stat st;
    fstat(fd, &st);
    size_t size = st.st_size;

    uint8_t *data = (uint8_t *)mmap(nullptr, size, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        log_error("mmap failed for corruption");
        close(fd);
        return false;
    }

    bool ok = true;

    if (ctx.cfg.passes & PASS_BITFLIP)
        ok = ok && corrupt_bitflip(ctx, data, size);

    if (ctx.cfg.passes & PASS_FTL)
        ok = ok && corrupt_ftl(ctx, data, size);

    if (ctx.cfg.passes & PASS_META)
        ok = ok && corrupt_metadata(ctx, data, size);

    if (ctx.cfg.passes & PASS_JPEG)
        ok = ok && corrupt_jpeg_targeted(ctx, data, size);

    msync(data, size, MS_SYNC);
    munmap(data, size);
    close(fd);

    return ok;
}

int main(int argc, char *argv[])
{
    SimContext ctx;
    auto &cfg = ctx.cfg;

    // Track which params were explicitly set (to allow profile + override)
    bool explicit_ber = false, explicit_bias = false, explicit_degraded = false;
    bool explicit_swap = false, explicit_zero = false;
    bool explicit_fat_break = false, explicit_dir_corrupt = false;
    bool explicit_passes = false;
    bool explicit_pre_corrupt = false, explicit_fragment = false;

    int opt;
    while ((opt = getopt_long(argc, argv, "s:o:vqh", long_options, nullptr)) != -1) {
        switch (opt) {
        case 's': cfg.populate.source_dir = optarg; break;
        case 'o': cfg.image_path = optarg; break;
        case 'v': cfg.verbosity++; break;
        case 'q': cfg.verbosity = -1; break;
        case 'h': print_usage(); return 0;
        case OPT_VERSION:
            printf("corrsim %s\n", CORRSIM_VERSION); return 0;
        case OPT_LIST_PROFILES:
            list_profiles(); return 0;

        // Image options
        case OPT_SIZE:  cfg.image_size_mb = strtoull(optarg, nullptr, 10); break;
        case OPT_SPC:   cfg.sectors_per_cluster = atoi(optarg); break;
        case OPT_LABEL: cfg.volume_label = optarg; break;

        // Population
        case OPT_FILE_COUNT: cfg.populate.file_count = atoi(optarg); break;
        case OPT_MAX_DEPTH:  cfg.populate.max_depth = atoi(optarg); break;
        case OPT_NO_RO:      cfg.populate.romanian_names = false; break;
        case OPT_NO_DCIM:    cfg.populate.dcim = false; break;

        // Fragmentation
        case OPT_FRAGMENT:       cfg.fragment.enabled = true; explicit_fragment = true; break;
        case OPT_FRAG_COUNT:     cfg.fragment.file_count = atoi(optarg); cfg.fragment.enabled = true; explicit_fragment = true; break;
        case OPT_FRAG_SIZE_MIN:  cfg.fragment.min_size = strtoul(optarg, nullptr, 10); cfg.fragment.enabled = true; explicit_fragment = true; break;
        case OPT_FRAG_SIZE_MAX:  cfg.fragment.max_size = strtoul(optarg, nullptr, 10); cfg.fragment.enabled = true; explicit_fragment = true; break;
        case OPT_FRAG_DELETE_PCT:cfg.fragment.delete_frac = strtod(optarg, nullptr) / 100.0; cfg.fragment.enabled = true; explicit_fragment = true; break;

        // Usage sim
        case OPT_SIM_OPS: cfg.usage.operations = atoi(optarg); break;
        case OPT_NO_SIM:  cfg.skip_sim = true; break;

        // Corruption
        case OPT_PRE_CORRUPT_FRAC:
            cfg.pre_corrupt_frac = strtod(optarg, nullptr);
            explicit_pre_corrupt = true; break;
        case OPT_PROFILE: cfg.profile = optarg; break;
        case OPT_PASSES:
            cfg.passes = (uint8_t)atoi(optarg);
            explicit_passes = true;
            break;
        case OPT_BER:
            cfg.bitflip.ber = strtod(optarg, nullptr);
            explicit_ber = true; break;
        case OPT_BIAS:
            cfg.bitflip.bias_0to1 = strtod(optarg, nullptr);
            explicit_bias = true; break;
        case OPT_DEGRADED:
            cfg.bitflip.degraded_frac = strtod(optarg, nullptr);
            explicit_degraded = true; break;
        case OPT_ERASE_BLOCK:
            cfg.bitflip.erase_block_size = strtoul(optarg, nullptr, 10); break;
        case OPT_FTL_BLOCK:
            cfg.ftl.block_size = strtoul(optarg, nullptr, 10); break;
        case OPT_SWAP_FRAC:
            cfg.ftl.swap_frac = strtod(optarg, nullptr);
            explicit_swap = true; break;
        case OPT_ZERO_FRAC:
            cfg.ftl.zero_frac = strtod(optarg, nullptr);
            explicit_zero = true; break;
        case OPT_FAT_BREAK:
            cfg.metadata.fat_chain_break_frac = strtod(optarg, nullptr);
            explicit_fat_break = true; break;
        case OPT_DIR_CORRUPT:
            cfg.metadata.dir_entry_corrupt_frac = strtod(optarg, nullptr);
            explicit_dir_corrupt = true; break;

        // General
        case OPT_SEED:     cfg.rng_seed = strtoull(optarg, nullptr, 10); break;
        case OPT_MANIFEST: cfg.manifest_path = optarg; break;
        case OPT_FULL_MANIFEST: cfg.full_manifest = true; break;

        default: print_usage(); return 1;
        }
    }

    // Validate required args
    if (cfg.populate.source_dir.empty() || cfg.image_path.empty()) {
        log_error("both -s <source_dir> and -o <output.img> are required");
        print_usage();
        return 1;
    }

    if (cfg.pre_corrupt_frac < 0.0 || cfg.pre_corrupt_frac > 1.0) {
        log_error("--pre-corrupt-frac must be between 0 and 1");
        return 1;
    }

    // Apply profile first, then let explicit args override
    if (!cfg.profile.empty()) {
        // Save explicit values
        auto saved_bf = cfg.bitflip;
        auto saved_ftl = cfg.ftl;
        auto saved_meta = cfg.metadata;
        auto saved_passes = cfg.passes;
        auto saved_pre_corrupt = cfg.pre_corrupt_frac;
        auto saved_fragment = cfg.fragment;

        if (!apply_profile(cfg.profile, cfg)) {
            log_error("unknown profile: %s", cfg.profile.c_str());
            list_profiles();
            return 1;
        }
        log_info(ctx, "Applied profile: %s", cfg.profile.c_str());

        // Restore explicit overrides
        if (explicit_ber) cfg.bitflip.ber = saved_bf.ber;
        if (explicit_bias) cfg.bitflip.bias_0to1 = saved_bf.bias_0to1;
        if (explicit_degraded) cfg.bitflip.degraded_frac = saved_bf.degraded_frac;
        if (explicit_swap) cfg.ftl.swap_frac = saved_ftl.swap_frac;
        if (explicit_zero) cfg.ftl.zero_frac = saved_ftl.zero_frac;
        if (explicit_fat_break) cfg.metadata.fat_chain_break_frac = saved_meta.fat_chain_break_frac;
        if (explicit_dir_corrupt) cfg.metadata.dir_entry_corrupt_frac = saved_meta.dir_entry_corrupt_frac;
        if (explicit_passes) cfg.passes = saved_passes;
        if (explicit_pre_corrupt) cfg.pre_corrupt_frac = saved_pre_corrupt;
        if (explicit_fragment) cfg.fragment = saved_fragment;
    }

    // Init RNG and logging
    rng_init(ctx);
    log_init(ctx);

    log_progress(ctx, "corrsim %s - FAT32 Corruption Simulator", CORRSIM_VERSION);
    log_progress(ctx, "RNG seed: %lu", (unsigned long)cfg.rng_seed);
    log_info(ctx, "Source: %s", cfg.populate.source_dir.c_str());
    log_info(ctx, "Output: %s (%luMB)", cfg.image_path.c_str(),
             (unsigned long)cfg.image_size_mb);
    if (!cfg.profile.empty())
        log_info(ctx, "Profile: %s", cfg.profile.c_str());
    log_info(ctx, "Passes: %s%s%s%s",
             (cfg.passes & PASS_BITFLIP) ? "bitflip " : "",
             (cfg.passes & PASS_FTL)     ? "ftl " : "",
             (cfg.passes & PASS_META)    ? "metadata " : "",
             (cfg.passes & PASS_JPEG)    ? "jpeg" : "");
    if (cfg.fragment.enabled)
        log_info(ctx, "Fragmentation: %d files, %.0f%% delete",
                 cfg.fragment.file_count, cfg.fragment.delete_frac * 100);
    if (cfg.pre_corrupt_frac > 0)
        log_info(ctx, "Pre-corruption: %.0f%% budget", cfg.pre_corrupt_frac * 100);

    // Pipeline
    if (!image_create(ctx)) return 1;
    if (!fragment_fill(ctx)) return 1;
    if (!file_populate(ctx)) return 1;
    if (!usage_simulate(ctx)) return 1;
    if (!ground_truth_record(ctx)) return 1;
    if (!pre_corruption_apply(ctx)) return 1;
    if (!corruption_apply(ctx)) return 1;
    if (!manifest_write(ctx)) return 1;

    log_progress(ctx, "Done. Image: %s", cfg.image_path.c_str());
    log_progress(ctx, "  Files recorded: %zu", ctx.truth.files.size());
    if (ctx.truth.pre_bits_flipped > 0 || ctx.truth.pre_fat_entries_corrupted > 0) {
        log_progress(ctx, "  Pre-corruption: %lu bits, %u FAT entries",
                     (unsigned long)ctx.truth.pre_bits_flipped,
                     ctx.truth.pre_fat_entries_corrupted);
    }
    log_progress(ctx, "  Bits flipped: %lu", (unsigned long)ctx.truth.total_bits_flipped);
    log_progress(ctx, "  FTL blocks: %u zeroed, %u swapped, %u wrong-data",
                 ctx.truth.blocks_zeroed, ctx.truth.blocks_swapped,
                 ctx.truth.blocks_wrong_data);
    log_progress(ctx, "  FAT entries corrupted: %u", ctx.truth.fat_entries_corrupted);
    log_progress(ctx, "  Dir entries corrupted: %u", ctx.truth.dir_entries_corrupted);

    log_close(ctx);
    return 0;
}
