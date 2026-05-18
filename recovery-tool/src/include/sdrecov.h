/*
 * sdrecov - SD Card JPEG Recovery Engine
 * Main header: data structures, constants, module interfaces.
 */
#pragma once

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <memory>
#include <cstdio>

/* ---- Constants --------------------------------------------------------- */

constexpr const char *SDRECOV_VERSION = "0.1.0";

constexpr uint32_t SECTOR_SIZE         = 512;
constexpr int      MAX_COMPONENTS      = 4;
constexpr int      MAX_HUFF_TABLES     = 4;
constexpr int      MAX_BLOCKS_PER_MCU  = 10;   /* ITU-T T.81: sum(Hi*Vi) <= 10 */
constexpr int      MAX_TEMPLATES       = 32;

/* FAT32 entry classification */
enum FatStatus : uint8_t {
    FAT_FREE = 0, FAT_VALID, FAT_EOF, FAT_BAD, FAT_CORRUPT, FAT_RESERVED
};

/* Cluster content types */
enum ContentType : uint8_t {
    CTYPE_UNKNOWN = 0, CTYPE_EMPTY, CTYPE_JPEG_HEADER, CTYPE_JPEG_SCAN,
    CTYPE_NON_JPEG, CTYPE_BAD_SECTOR, CTYPE_MP4_HEADER
};

/* Cluster feature flags */
enum ClusterFlags : uint8_t {
    CF_HAS_SOI = 0x01, CF_HAS_EOI = 0x02, CF_HAS_SOS = 0x04,
    CF_HAS_DQT = 0x08, CF_HAS_RST = 0x10, CF_IS_ZERO = 0x20
};

/* Seed sources */
enum SeedSource : uint8_t {
    SEED_DIR_ENTRY = 0x01, SEED_SIGNATURE_SCAN = 0x02, SEED_BOTH = 0x03
};

/* Seed confidence */
enum Confidence : uint8_t {
    CONF_SPECULATIVE = 0, CONF_LOW, CONF_MEDIUM, CONF_HIGH
};

/* JPEG mode */
enum JpegMode : uint8_t {
    JPEG_BASELINE = 0, JPEG_PROGRESSIVE, JPEG_UNKNOWN
};

/* Huffman validation error types */
enum HuffError : uint8_t {
    HUFF_OK = 0, HUFF_ERR_DC, HUFF_ERR_AC, HUFF_ERR_QA,
    HUFF_ERR_EOF, HUFF_ERR_RST, HUFF_ERR_MARKER
};

/* ---- Data Structures --------------------------------------------------- */

struct PartitionEntry {
    uint8_t  status = 0;
    uint8_t  type = 0;
    uint32_t start_lba = 0;
    uint32_t size_sectors = 0;
};

struct Fat32Geometry {
    uint32_t bytes_per_sector = 0;
    uint32_t sectors_per_cluster = 0;
    uint32_t bytes_per_cluster = 0;
    uint32_t reserved_sectors = 0;
    uint32_t num_fats = 0;
    uint32_t sectors_per_fat = 0;
    uint32_t root_cluster = 0;
    uint32_t total_sectors = 0;
    uint32_t total_clusters = 0;
    uint64_t partition_offset = 0;
    uint64_t fat1_offset = 0;
    uint64_t fat2_offset = 0;
    uint64_t data_offset = 0;
};

struct FatTables {
    std::vector<uint32_t> fat1;
    std::vector<uint32_t> fat2;
    std::vector<uint32_t> merged;
    std::vector<FatStatus> status;

    void resize(uint32_t count) {
        fat1.resize(count, 0);
        fat2.resize(count, 0);
        merged.resize(count, 0);
        status.resize(count, FAT_FREE);
    }
    uint32_t count() const { return (uint32_t)fat1.size(); }
};

struct ClusterFeature {
    float       entropy = 0;
    uint16_t    ff00_count = 0;
    uint8_t     flags = 0;
    ContentType content_type = CTYPE_UNKNOWN;
};

struct HuffTable {
    int     maxcode[18] = {};       /* largest code at each bit length, -1 if none */
    int     valoffset[18] = {};     /* symbol offset */
    uint8_t huffval[256] = {};      /* symbol values */
    int     look_nbits[256] = {};   /* 8-bit lookahead: code length (0 = >8 bits) */
    uint8_t look_sym[256] = {};     /* 8-bit lookahead: symbol value */
    int     max_code_length = 0;
    int     num_symbols = 0;
};

/* Progressive scan configuration (one per SOS marker) */
struct ScanConfig {
    uint8_t  num_components = 0;
    uint8_t  comp_index[MAX_COMPONENTS] = {};
    uint8_t  dc_tbl[MAX_COMPONENTS] = {};
    uint8_t  ac_tbl[MAX_COMPONENTS] = {};
    uint8_t  ss = 0;     /* spectral selection start */
    uint8_t  se = 63;    /* spectral selection end */
    uint8_t  ah = 0;     /* successive approximation high */
    uint8_t  al = 0;     /* successive approximation low */
};

struct McuConfig {
    uint8_t  blocks_per_mcu = 0;
    uint8_t  block_comp[MAX_BLOCKS_PER_MCU] = {};
    uint8_t  block_dc_tbl[MAX_BLOCKS_PER_MCU] = {};
    uint8_t  block_ac_tbl[MAX_BLOCKS_PER_MCU] = {};
    uint32_t total_mcus = 0;
    uint16_t mcu_width = 0;
    uint16_t mcu_height = 0;
    uint16_t restart_interval = 0;
    uint16_t image_width = 0;
    uint16_t image_height = 0;
    uint8_t  num_components = 0;
    JpegMode jpeg_mode = JPEG_UNKNOWN;

    /* Progressive: scan parameters (populated for SOF2 images) */
    uint8_t    num_scans = 0;
    ScanConfig scans[32];
};

struct HuffCheckpoint {
    uint64_t bit_buffer = 0;
    int      bits_left = 0;
    size_t   byte_pos = 0;
    int32_t  dc_pred[MAX_COMPONENTS] = {};
    uint32_t mcu_count = 0;
    uint8_t  block_index = 0;
    uint16_t rst_counter = 0;
    uint32_t mcus_to_restart = 0;

    /* Progressive state */
    uint8_t  current_scan = 0;
    uint8_t  scan_ss = 0;
    uint8_t  scan_se = 63;
    uint8_t  scan_ah = 0;
    uint8_t  scan_al = 0;
    uint16_t eob_run = 0;    /* EOBn run counter for progressive AC scans */
};

struct HuffResult {
    bool     passed = false;
    size_t   offset = 0;
    HuffError error_type = HUFF_OK;
    uint32_t mcu_count = 0;
};

struct JpegTemplate {
    std::vector<uint8_t> header_bytes;  /* raw SOI..SOS */
    uint8_t  dqt_luma[64] = {};
    uint8_t  dqt_chroma[64] = {};
    uint16_t width = 0, height = 0;
    uint8_t  subsampling = 0;
    uint16_t restart_interval = 0;
    std::string camera;
    HuffTable dc_tables[MAX_HUFF_TABLES];
    HuffTable ac_tables[MAX_HUFF_TABLES];
    McuConfig mcu_config;
};

struct Seed {
    uint32_t   start_cluster = 0;
    uint32_t   expected_size = 0;
    uint32_t   expected_clusters = 0;
    SeedSource source = SEED_DIR_ENTRY;
    Confidence confidence = CONF_SPECULATIVE;
    uint8_t    camera_id = 0;
    JpegMode   jpeg_mode = JPEG_UNKNOWN;
    bool       has_thumbnail = false;
    uint32_t   thumbnail_offset = 0;
    uint32_t   thumbnail_size = 0;
    uint16_t   soi_offset = 0;       /* byte offset of SOI within start_cluster */
    std::string filename;
};

/* RAII wrapper for mmap'd file */
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;
    MappedFile(MappedFile &&other) noexcept;
    MappedFile &operator=(MappedFile &&other) noexcept;

    bool open(const char *path);
    const uint8_t *data() const { return data_; }
    size_t size() const { return size_; }
    explicit operator bool() const { return data_ != nullptr; }
private:
    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
};

struct DiskImage {
    MappedFile primary;
    MappedFile secondary;   /* optional second read */
    Fat32Geometry geo;

    const uint8_t *data() const { return primary.data(); }
    size_t size() const { return primary.size(); }
    const uint8_t *cluster_ptr(uint32_t cluster) const;
};

struct FeatureFlags {
    /* Chain building */
    bool fast_path          = true;
    bool chain_repair       = true;
    bool sequential_scan    = true;
    bool seam_detection     = true;
    bool chain_adequacy     = true;
    bool size_estimation    = true;
    bool oversize_terminate = true;

    /* Candidate filtering */
    bool boundary_check     = true;
    bool mcu_rate_filter    = true;
    bool mcu_progress_score = true;
    bool ff00_prefilter     = true;
    bool entropy_filter     = true;
    bool dc_bounds          = true;
    bool rst_expectancy     = true;

    /* Header repair */
    bool dht_fallback       = true;
    bool header_graft       = true;
    bool annex_k_retry      = true;

    /* Validation */
    bool lenient_ff         = true;
    bool tolerant_validate  = true;
    bool rst_recovery       = true;

    /* Post-processing */
    bool thumbnail_validate = true;
    bool template_tables    = true;

    /* Seed discovery */
    bool mid_cluster_soi    = true;

    void enable_all()  { *this = FeatureFlags{}; }
    void disable_all() {
        fast_path = chain_repair = sequential_scan = seam_detection = false;
        chain_adequacy = size_estimation = oversize_terminate = false;
        boundary_check = mcu_rate_filter = mcu_progress_score = false;
        ff00_prefilter = entropy_filter = dc_bounds = rst_expectancy = false;
        dht_fallback = header_graft = annex_k_retry = false;
        lenient_ff = tolerant_validate = rst_recovery = false;
        thumbnail_validate = template_tables = false;
        mid_cluster_soi = false;
    }
};

/* Map feature name string to pointer into FeatureFlags struct */
struct FeatureFlagEntry { const char *name; bool FeatureFlags::*ptr; };
inline const FeatureFlagEntry FEATURE_FLAG_TABLE[] = {
    {"fast-path",          &FeatureFlags::fast_path},
    {"chain-repair",       &FeatureFlags::chain_repair},
    {"sequential-scan",    &FeatureFlags::sequential_scan},
    {"seam-detection",     &FeatureFlags::seam_detection},
    {"chain-adequacy",     &FeatureFlags::chain_adequacy},
    {"size-estimation",    &FeatureFlags::size_estimation},
    {"oversize-terminate", &FeatureFlags::oversize_terminate},
    {"boundary-check",     &FeatureFlags::boundary_check},
    {"mcu-rate-filter",    &FeatureFlags::mcu_rate_filter},
    {"mcu-progress-score", &FeatureFlags::mcu_progress_score},
    {"ff00-prefilter",     &FeatureFlags::ff00_prefilter},
    {"entropy-filter",     &FeatureFlags::entropy_filter},
    {"dc-bounds",          &FeatureFlags::dc_bounds},
    {"rst-expectancy",     &FeatureFlags::rst_expectancy},
    {"dht-fallback",       &FeatureFlags::dht_fallback},
    {"header-graft",       &FeatureFlags::header_graft},
    {"annex-k-retry",      &FeatureFlags::annex_k_retry},
    {"lenient-ff",         &FeatureFlags::lenient_ff},
    {"tolerant-validate",  &FeatureFlags::tolerant_validate},
    {"rst-recovery",       &FeatureFlags::rst_recovery},
    {"thumbnail-validate", &FeatureFlags::thumbnail_validate},
    {"template-tables",    &FeatureFlags::template_tables},
    {"mid-cluster-soi",    &FeatureFlags::mid_cluster_soi},
    {nullptr, nullptr}
};

struct RecoveryContext {
    DiskImage    disk;
    FatTables    fat;
    std::vector<ClusterFeature> cluster_map;
    std::vector<Seed>           seeds;
    std::vector<JpegTemplate>   templates;
    std::vector<float>          claimed_score; /* score per cluster, 0 = unclaimed */
    std::vector<uint8_t>        cluster_refcount; /* 0=unclaimed, 1=single, 2+=cross-linked */
    float                       fat_confidence = 1.0f; /* fraction of valid FAT entries */

    /* Feature flags */
    FeatureFlags features;

    /* Config */
    int         threads = 4;
    int         max_candidates = 50;
    int         max_bit_flips = 1;
    int         search_radius = 64;
    float       min_score = 0.3f;
    float       high_confidence = 0.85f;
    int         verbosity = 1;
    int         seeds_offset = 0;     /* skip first N seeds */
    int         seeds_limit = 0;      /* process at most N seeds (0 = all) */
    int         beam_width = 1;       /* beam search: number of chains to keep alive */
    int         max_backtracks = 5;   /* greedy mode: max backtracks per seed */

    /* Search strategy:
     *   "greedy" - pick best candidate each step, lazy backtrack (default, fast)
     *   "beam"   - keep beam_width partial chains alive, prune at each step
     *   "full"   - exhaustive DFS, try ALL branches, keep best chain (slow!)
     */
    enum SearchMode { SEARCH_GREEDY = 0, SEARCH_BEAM, SEARCH_FULL } search_mode = SEARCH_GREEDY;
    int         geo_cluster_size = 0;  /* override: bytes per cluster (0 = auto from boot sector) */
    int         geo_data_offset = 0;   /* override: byte offset to data area (0 = auto) */
    std::string output_dir;
    FILE       *debug_log = nullptr;

    ~RecoveryContext() { if (debug_log) fclose(debug_log); }
};

/* ---- Module interfaces ------------------------------------------------- */

/* log.cpp - verbosity levels: 0=progress, 1=info(-v), 2=detail(-vv), 3=debug(-vvv) */
void log_init(RecoveryContext &ctx);
void log_progress(const RecoveryContext &ctx, const char *fmt, ...);  /* level 0: always */
void log_info(const RecoveryContext &ctx, const char *fmt, ...);      /* level 1: -v */
void log_detail(const RecoveryContext &ctx, const char *fmt, ...);    /* level 2: -vv */
void log_debug(const RecoveryContext &ctx, const char *fmt, ...);     /* level 3: -vvv */
void log_error(const char *fmt, ...);

/* partition.cpp */
int  partition_detect(const uint8_t *image, size_t size,
                      std::vector<PartitionEntry> &entries);
uint64_t partition_get_offset(const uint8_t *image, size_t size, int partition_num);

/* fat32.cpp */
bool fat32_parse(DiskImage &disk, uint64_t partition_offset);
bool fat32_read_tables(const DiskImage &disk, FatTables &fat);

/* fat32_autodetect.cpp */
struct Fat32AutodetectResult {
    Fat32Geometry geo;
    float         confidence = 0;
    bool          valid = false;
};
Fat32AutodetectResult fat32_autodetect(const uint8_t *image, size_t image_size,
                                        uint64_t partition_offset);

/* fat_merge.cpp */
void fat_merge(FatTables &fat, uint32_t total_clusters);
void fat_build_refcount(const FatTables &fat, uint32_t total_clusters,
                        std::vector<uint8_t> &refcount);
int  fat_bitflip_candidates(uint32_t entry, uint32_t max_cluster,
                             uint32_t *out, int max_out);

/* cluster_map.cpp */
void cluster_map_build(const DiskImage &disk, std::vector<ClusterFeature> &map);

/* seed.cpp */
void seed_build(RecoveryContext &ctx);

/* jpeg_parse.cpp */
bool jpeg_parse_header(const uint8_t *data, size_t len, JpegTemplate &tmpl);
std::vector<uint8_t> inject_dri(const std::vector<uint8_t> &header_bytes,
                                 uint16_t restart_interval);
bool jpeg_extract_thumbnail(const uint8_t *data, size_t len,
                             uint32_t &thumb_offset, uint32_t &thumb_size);
int  dqt_distance(const uint8_t a[64], const uint8_t b[64]);
void template_library_build(RecoveryContext &ctx);
int  template_find_best(const RecoveryContext &ctx, const uint8_t *cluster_data, size_t len);

/* huffman.cpp */
bool huff_table_build(HuffTable &ht, const uint8_t bits[16],
                      const uint8_t *huffval, int num_symbols);
HuffResult huff_validate_cluster(const uint8_t *data, size_t len,
                                  const McuConfig &cfg,
                                  const HuffTable dc_tables[],
                                  const HuffTable ac_tables[],
                                  HuffCheckpoint &state);
uint32_t huff_validate_tolerant(const uint8_t *data, size_t len,
                                const McuConfig &cfg,
                                const HuffTable dc_tables[],
                                const HuffTable ac_tables[],
                                int max_resync = 10);
HuffResult huff_validate_one_mcu(const uint8_t *data, size_t len,
                                  const McuConfig &cfg,
                                  const HuffTable dc_tables[],
                                  const HuffTable ac_tables[],
                                  HuffCheckpoint &state);

/* bitstream.cpp */
struct BitStream;
void   bs_init(BitStream &bs, const uint8_t *data, size_t len);
int    bs_peek(BitStream &bs, int nbits);
void   bs_skip(BitStream &bs, int nbits);
int    bs_read(BitStream &bs, int nbits);
size_t bs_byte_offset(const BitStream &bs);
void   bs_align(BitStream &bs);
bool   bs_consume_restart(BitStream &bs, int expected_rst);

/* candidates.cpp */
int enumerate_candidates(uint32_t current, const RecoveryContext &ctx,
                         const Seed &seed, int chain_length,
                         uint32_t *out_candidates, float *out_priorities,
                         int max_candidates);

/* boundary_check.cpp */
float boundary_coherence(const HuffCheckpoint &pre_state,
                          const HuffCheckpoint &post_state,
                          uint32_t new_mcus);
float first_mcu_boundary_check(const uint8_t *data, size_t len,
                                const McuConfig &cfg,
                                const HuffTable dc_tables[],
                                const HuffTable ac_tables[],
                                const HuffCheckpoint &pre_state);

/* scoring.cpp */
float score_candidate(const HuffResult &result,
                      const HuffCheckpoint &pre_state,
                      const HuffCheckpoint &post_state,
                      uint32_t candidate, uint32_t current, const Seed &seed,
                      const ClusterFeature *features, const FatTables &fat);
float evaluate_chain_quality(const RecoveryContext &ctx, const Seed &seed,
                              const std::vector<uint32_t> &clusters,
                              float avg_score, uint32_t mcus, bool complete,
                              float thumb_confidence);

/* thumbnail_validate.cpp */
float thumbnail_validate(const RecoveryContext &ctx, const Seed &seed,
                          const uint8_t *recovered_data, size_t recovered_len);

/* rst_recovery.cpp */
uint32_t rst_skip_and_resume(const uint8_t *data, size_t len,
                              const McuConfig &cfg,
                              const HuffTable dc_tables[],
                              const HuffTable ac_tables[],
                              HuffCheckpoint &state,
                              size_t error_offset);

/* header_graft.cpp */
bool header_graft(RecoveryContext &ctx, uint32_t start_cluster,
                  JpegTemplate &tmpl_out, size_t &entropy_offset);

/* engine.cpp */
/* Chain result from DFS search */
struct ChainResult {
    std::vector<uint32_t> clusters;
    float score = 0;
    uint32_t mcus_recovered = 0;
    uint32_t total_mcus = 0;
    bool complete = false;
    float thumb_confidence = -1.0f;
    bool grafted = false;
    std::vector<uint8_t> graft_header;
    size_t entropy_offset = 0;
    bool has_validation_gap = false;
};

/* A chain variant: one possible recovery path for a seed */
struct ChainVariant {
    ChainResult chain;
    std::string tag;         /* "fat", "sequential", "seam-repaired", "dfs", "dfs-alt" */
    float confidence = 0.0f; /* 0.0-1.0, higher = more likely correct */
};

/* A branch point for DFS backtracking */
struct BranchPoint {
    int step;
    HuffCheckpoint state;
    std::vector<uint32_t> chain_so_far;
    float score_so_far;
    std::vector<uint32_t> candidates;
    std::vector<float> cand_scores;
    size_t chain_buf_size;
};

/* Standard Annex K tables */
extern HuffTable g_std_dc[2], g_std_ac[2];
extern bool g_std_tables_built;
void build_standard_tables();

/* engine.cpp */
int engine_recover(RecoveryContext &ctx);

/* dfs_explore.cpp */
void dfs_print_filter_stats(const RecoveryContext &ctx);

/* chain_validate.cpp */
float validate_whole_chain(const RecoveryContext &ctx, const JpegTemplate &tmpl,
                            ChainResult &chain);
float evaluate_chain(const RecoveryContext &ctx, const Seed &seed,
                      const ChainResult &chain);

/* file_writer.cpp */
bool write_recovered(const RecoveryContext &ctx, const Seed &seed,
                      const ChainResult &chain, int file_num);
bool write_recovered_variant(const RecoveryContext &ctx, const Seed &seed,
                              const ChainVariant &variant, int file_num,
                              int variant_idx);
bool write_recovered_with_rst(const RecoveryContext &ctx, const Seed &seed,
                               const ChainResult &chain, int file_num);

/* gap_detect.cpp */
bool is_gap_cluster(const RecoveryContext &ctx, uint32_t cluster);
std::vector<bool> detect_gaps_by_validation(const RecoveryContext &ctx,
    const std::vector<uint32_t> &clusters, const JpegTemplate &tmpl, size_t header_len);

/* fast_path.cpp */
struct FastPathResult {
    std::vector<ChainVariant> variants; /* all plausible chains found */
    float best_eval = -1.0f;
    bool should_return = false;
};
FastPathResult try_fast_path(RecoveryContext &ctx, const Seed &seed,
                              const JpegTemplate &tmpl,
                              const std::vector<uint32_t> &header_chain,
                              size_t header_len, bool was_grafted,
                              const std::vector<uint8_t> &graft_header_bytes,
                              size_t graft_entropy_off,
                              uint32_t effective_expected_size,
                              uint32_t estimated_min_size,
                              uint32_t estimated_max_size);

/* dfs_explore.cpp */
ChainResult explore_path(RecoveryContext &ctx, const JpegTemplate &tmpl,
                          const Seed &seed,
                          std::vector<uint32_t> chain_clusters,
                          HuffCheckpoint state, float score_accum,
                          std::vector<BranchPoint> &branches,
                          std::vector<uint8_t> &chain_buf,
                          int max_chain_len);
