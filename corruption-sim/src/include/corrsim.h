/*
 * corrsim - FAT32 Corruption Simulator
 * Main header: data structures, constants, module interfaces.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <random>
#include <cstdio>
#include <functional>

/* ---- Constants --------------------------------------------------------- */

constexpr const char *CORRSIM_VERSION = "0.1.0";

constexpr uint32_t SECTOR_SIZE = 512;
constexpr uint32_t FAT32_ENTRY_MASK = 0x0FFFFFFF;
constexpr uint32_t FAT32_EOF_MIN   = 0x0FFFFFF8;
constexpr uint32_t FAT32_BAD       = 0x0FFFFFF7;
constexpr uint32_t FAT32_FREE      = 0x00000000;

/* Corruption pass bitmask */
enum CorruptionPass : uint8_t {
    PASS_BITFLIP = 0x01,
    PASS_FTL     = 0x02,
    PASS_META    = 0x04,
    PASS_JPEG    = 0x08,
    PASS_ALL     = 0x0F,
};

/* ---- Configuration Structures ------------------------------------------ */

struct BitflipConfig {
    double   ber              = 1e-5;    // bit error rate
    double   bias_0to1        = 0.80;    // fraction of flips that go 0->1
    double   degraded_frac    = 0.10;    // fraction of erase blocks degraded
    double   degraded_mult    = 10.0;    // BER multiplier in degraded blocks
    uint32_t erase_block_size = 131072;  // 128KB default
    bool     msb_bias         = true;    // 2-3x more errors in MSB pages (MLC)
};

struct FtlConfig {
    double   swap_frac       = 0.01;     // fraction of blocks swapped
    double   zero_frac       = 0.005;    // fraction of blocks zeroed
    double   wrong_data_frac = 0.005;    // blocks filled with wrong-address data
    uint32_t block_size      = 4194304;  // 4MB SDHC allocation group
};

struct MetadataConfig {
    double fat_chain_break_frac  = 0.005;  // fraction of FAT entries corrupted
    int    fat_desync_entries    = 10;      // contiguous FAT1 entries to desync
    double dir_entry_corrupt_frac = 0.05;  // fraction of dir entries corrupted
    bool   lfn_cjk_corruption   = true;    // produce CJK chars in LFN
    bool   cross_link           = true;    // create cross-linked clusters
    bool   premature_eod        = true;    // create 0x00 end-of-directory
    int    deleted_marks        = 5;       // entries marked 0xE5
};

struct PopulateConfig {
    std::string source_dir;
    int  file_count     = 200;
    int  max_depth      = 4;
    bool romanian_names = true;
    bool dcim           = true;
};

struct UsageSimConfig {
    int    operations  = 50;
    double delete_prob = 0.30;
    double move_prob   = 0.20;
    double copy_prob   = 0.50;
};

struct FragmentConfig {
    bool     enabled     = false;
    int      file_count  = 500;
    uint32_t min_size    = 2048;   // 2KB
    uint32_t max_size    = 65536;  // 64KB
    double   delete_frac = 0.60;
};

struct JpegCorruptConfig {
    double dht_corrupt_prob       = 0;   // per-file probability of DHT bit flips
    int    dht_max_flips          = 2;   // max bit flips per DHT segment
    double dqt_corrupt_prob       = 0;   // per-file probability of DQT bit flips
    int    dqt_max_flips          = 2;
    double sof_corrupt_prob       = 0;   // per-file probability of SOF dim corruption
    double stuffing_corrupt_prob  = 0;   // per-file probability of FF00 pair corruption
    int    stuffing_max_flips     = 3;   // max FF00 pairs to corrupt per file
    double rst_corrupt_prob       = 0;   // per-file probability of RST marker corruption
    double partial_zero_prob      = 0;   // per-file probability of zeroing first/last 512B of a cluster
    double misalign_prob          = 0;   // per-file probability of shifting data within first cluster
    int    misalign_max_offset    = 512; // max shift in bytes (SOI moves to this offset)
    double sos_corrupt_prob       = 0;   // per-file probability of SOS table index corruption
};

struct SimConfig {
    std::string image_path;
    uint64_t    image_size_mb       = 512;
    uint32_t    sectors_per_cluster = 8;
    std::string volume_label        = "TESTDISK";
    uint64_t    rng_seed            = 0;   // 0 = random
    int         verbosity           = 0;
    uint8_t     passes              = PASS_ALL;
    std::string profile;
    std::string manifest_path;
    bool        full_manifest       = false;
    bool        skip_sim            = false;
    double      pre_corrupt_frac    = 0.0;  // 0 = no pre-corruption

    PopulateConfig     populate;
    UsageSimConfig     usage;
    FragmentConfig     fragment;
    BitflipConfig      bitflip;
    FtlConfig          ftl;
    MetadataConfig     metadata;
    JpegCorruptConfig  jpeg;
};

/* ---- FAT32 Geometry ---------------------------------------------------- */

struct Fat32Geo {
    uint32_t bytes_per_sector     = 0;
    uint32_t sectors_per_cluster  = 0;
    uint32_t bytes_per_cluster    = 0;
    uint32_t reserved_sectors     = 0;
    uint32_t num_fats             = 0;
    uint32_t sectors_per_fat      = 0;
    uint32_t root_cluster         = 0;
    uint32_t total_sectors        = 0;
    uint32_t total_clusters       = 0;
    uint64_t fat1_offset          = 0;
    uint64_t fat2_offset          = 0;
    uint64_t data_offset          = 0;
};

/* ---- Ground Truth Structures ------------------------------------------- */

struct FileRecord {
    std::string original_path;       // path on host filesystem
    std::string image_path;          // path inside FAT32 image
    std::vector<uint32_t> clusters;  // cluster chain
    uint32_t    file_size    = 0;
    uint64_t    data_offset  = 0;    // byte offset of first cluster in image
    std::string sha256;
    std::string sha256_source;       // SHA-256 of original source file (clean reference)
    bool        was_deleted  = false;
    bool        was_moved    = false;
    std::string moved_from;
};

struct MutationRecord {
    std::string type;       // "bitflip", "block_zero", "block_swap", etc.
    uint64_t    offset;     // byte offset in image
    uint32_t    length;     // bytes affected
    std::string detail;     // human-readable description
};

struct GroundTruth {
    Fat32Geo geo;
    std::vector<FileRecord>     files;
    std::vector<FileRecord>     deleted_files;
    std::vector<MutationRecord> mutations;

    // Aggregated stats (post-corruption)
    uint64_t total_bits_flipped     = 0;
    uint32_t blocks_zeroed          = 0;
    uint32_t blocks_swapped         = 0;
    uint32_t blocks_wrong_data      = 0;
    uint32_t fat_entries_corrupted  = 0;
    uint32_t dir_entries_corrupted  = 0;

    // Pre-corruption stats (data-region only, before usage sim)
    uint64_t pre_bits_flipped       = 0;
    uint32_t pre_blocks_zeroed      = 0;
    uint32_t pre_blocks_swapped     = 0;
    uint32_t pre_blocks_wrong_data  = 0;
    uint32_t pre_fat_entries_corrupted = 0;
};

/* ---- Runtime Context --------------------------------------------------- */

/* Tracks a file placement from populate or usage-sim copy */
struct PlacedFile {
    std::string image_path;   // path inside FAT32 image
    std::string source_path;  // path on host filesystem
};

/* Tracks a deletion or move during usage simulation */
struct SimOperation {
    std::string type;         // "delete", "move", "copy"
    std::string path;         // affected image path
    std::string dest;         // destination (for move/copy)
    std::string source;       // host source (for copy)
    int op_index = 0;         // operation sequence number
};

struct SimContext {
    SimConfig    cfg;
    GroundTruth  truth;
    std::mt19937_64 rng;
    FILE        *debug_log = nullptr;

    // Files currently in the image (updated during usage sim)
    std::vector<std::string> image_files;
    // Source files available for copying
    std::vector<std::string> source_files;

    // Tracking for ground truth
    std::vector<PlacedFile>   placed_files;   // all files ever placed
    std::vector<SimOperation> sim_ops;        // operations during usage sim
};

/* ---- Logging (util/log.cpp) -------------------------------------------- */

void log_init(SimContext &ctx);
void log_close(SimContext &ctx);
void log_progress(const SimContext &ctx, const char *fmt, ...);
void log_info(const SimContext &ctx, const char *fmt, ...);
void log_detail(const SimContext &ctx, const char *fmt, ...);
void log_debug(const SimContext &ctx, const char *fmt, ...);
void log_error(const char *fmt, ...);

/* ---- Subprocess (util/subprocess.cpp) ---------------------------------- */

struct SubprocessResult {
    int exit_code;
    std::string out;
    std::string err;
};

SubprocessResult run_cmd(const std::string &cmd, int verbosity = 0);

/* ---- RNG (util/rng.cpp) ------------------------------------------------ */

void        rng_init(SimContext &ctx);
double      rng_uniform(SimContext &ctx, double lo, double hi);
int         rng_int(SimContext &ctx, int lo, int hi);  // inclusive
bool        rng_bernoulli(SimContext &ctx, double p);
uint64_t    rng_geometric(SimContext &ctx, double p);
std::string rng_pick(SimContext &ctx, const std::vector<std::string> &v);
void        rng_shuffle(SimContext &ctx, std::vector<std::string> &v);
void        rng_shuffle_u32(SimContext &ctx, std::vector<uint32_t> &v);

/* ---- JSON Writer (util/json_writer.cpp) -------------------------------- */

class JsonWriter {
    FILE *f_;
    int indent_ = 0;
    bool need_comma_ = false;
    std::vector<bool> comma_stack_;

    void write_indent();
    void maybe_comma();
public:
    explicit JsonWriter(FILE *f) : f_(f) {}

    void begin_object();
    void end_object();
    void begin_array();
    void end_array();
    void key(const char *k);
    void value_string(const std::string &v);
    void value_int(int64_t v);
    void value_uint(uint64_t v);
    void value_double(double v, int precision = 6);
    void value_bool(bool v);
    void value_null();
};

/* ---- Folder Name Data (data/folder_names.cpp) -------------------------- */

const std::vector<std::string> &folder_names_camera();
const std::vector<std::string> &folder_names_english();
const std::vector<std::string> &folder_names_romanian();
const std::vector<std::string> &file_name_prefixes();
const std::vector<std::string> &file_name_descriptive();

/* ---- Profiles (data/profiles.cpp) -------------------------------------- */

bool apply_profile(const std::string &name, SimConfig &cfg);
void list_profiles();

/* ---- Pipeline Stages --------------------------------------------------- */

// Stage 1: Create blank FAT32 image
bool image_create(SimContext &ctx);

// Stage 1b: Forced fragmentation (create/delete text spacers)
bool fragment_fill(SimContext &ctx);

// Stage 2: Populate with files/folders
bool file_populate(SimContext &ctx);

// Stage 3: Simulate user operations
bool usage_simulate(SimContext &ctx);

// Stage 4: Record ground truth (parses FAT32 natively)
bool ground_truth_record(SimContext &ctx);

// Stage 4b: Pre-corruption (data-region only, before usage sim)
bool pre_corruption_apply(SimContext &ctx);

// Stage 5: Apply corruption
bool corrupt_bitflip(SimContext &ctx, uint8_t *data, size_t size,
                     uint64_t region_start = 0, uint64_t region_end = UINT64_MAX);
bool corrupt_ftl(SimContext &ctx, uint8_t *data, size_t size,
                 uint64_t region_start = 0, uint64_t region_end = UINT64_MAX);
bool corrupt_fat_chains(SimContext &ctx, uint8_t *data, size_t size);
bool corrupt_metadata(SimContext &ctx, uint8_t *data, size_t size);
bool corruption_apply(SimContext &ctx);

// Stage 5b: JPEG-targeted corruption (operates after ground truth recording)
bool corrupt_jpeg_targeted(SimContext &ctx, uint8_t *data, size_t size);

// Write ground truth manifest
bool manifest_write(const SimContext &ctx);

/* ---- FAT32 Helpers (used by ground_truth and metadata corruption) ------ */

bool     fat32_parse_geo(const uint8_t *data, size_t size, Fat32Geo &geo);
uint32_t fat32_read_entry(const uint8_t *data, const Fat32Geo &geo, uint32_t cluster);
void     fat32_write_entry(uint8_t *data, const Fat32Geo &geo, uint32_t cluster,
                           uint32_t value, int fat_num = 0);
uint64_t fat32_cluster_offset(const Fat32Geo &geo, uint32_t cluster);

inline uint16_t read16(const uint8_t *p) { return p[0] | (p[1] << 8); }
inline uint32_t read32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
inline void write16(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
inline void write32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
