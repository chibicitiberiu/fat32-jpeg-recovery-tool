/*
 * test_huffman.cpp - Test the Huffman validator against real JPEG files
 *
 * Tests:
 * 1. Parse a known-good JPEG, validate all clusters -> should pass
 * 2. Parse same JPEG, corrupt a cluster in the middle -> should detect fragmentation
 * 3. Append random data after real JPEG data -> should detect fragmentation point
 */
#include "sdrecov.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static std::vector<uint8_t> read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return {}; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(sz);
    fread(data.data(), 1, sz, f);
    fclose(f);
    return data;
}

static bool test_good_jpeg(const char *path)
{
    printf("Test 1: Validate good JPEG: %s\n", path);

    auto data = read_file(path);
    if (data.empty()) return false;

    JpegTemplate tmpl;
    if (!jpeg_parse_header(data.data(), data.size(), tmpl)) {
        printf("  FAIL: cannot parse JPEG header\n");
        return false;
    }

    printf("  Parsed: %dx%d, %d components, %d blocks/MCU, %u total MCUs, mode=%s\n",
           tmpl.mcu_config.image_width, tmpl.mcu_config.image_height,
           tmpl.mcu_config.num_components, tmpl.mcu_config.blocks_per_mcu,
           tmpl.mcu_config.total_mcus,
           tmpl.mcu_config.jpeg_mode == JPEG_BASELINE ? "baseline" : "progressive");
    printf("  Restart interval: %d\n", tmpl.mcu_config.restart_interval);
    printf("  Header size: %zu bytes\n", tmpl.header_bytes.size());

    /* Validate entropy data after header */
    const uint8_t *entropy = data.data() + tmpl.header_bytes.size();
    size_t entropy_len = data.size() - tmpl.header_bytes.size();

    HuffCheckpoint state = {};
    if (tmpl.mcu_config.restart_interval > 0)
        state.mcus_to_restart = tmpl.mcu_config.restart_interval;

    HuffResult result = huff_validate_cluster(
        entropy, entropy_len,
        tmpl.mcu_config, tmpl.dc_tables, tmpl.ac_tables, state);

    printf("  Result: %s, %u MCUs validated, offset=%zu\n",
           result.passed ? "PASS" : "FAIL",
           result.mcu_count, result.offset);

    if (!result.passed) {
        const char *err_names[] = {"OK","DC","AC","QA","EOF","RST","MARKER"};
        printf("  Error: %s at offset %zu\n", err_names[result.error_type], result.offset);
    }

    if (result.passed && result.mcu_count == tmpl.mcu_config.total_mcus) {
        printf("  OK: all %u MCUs validated\n", result.mcu_count);
        return true;
    } else if (result.passed) {
        printf("  OK: %u/%u MCUs (ran out of data, expected for truncated files)\n",
               result.mcu_count, tmpl.mcu_config.total_mcus);
        return true;
    }
    return false;
}

static bool test_corrupt_jpeg(const char *path)
{
    printf("\nTest 2: Validate JPEG with corrupted middle: %s\n", path);

    auto data = read_file(path);
    if (data.empty()) return false;

    JpegTemplate tmpl;
    if (!jpeg_parse_header(data.data(), data.size(), tmpl)) return false;

    /* Corrupt the middle of entropy data with random bytes */
    size_t entropy_start = tmpl.header_bytes.size();
    size_t entropy_len = data.size() - entropy_start;
    size_t corrupt_offset = entropy_len / 2;

    auto corrupted = data;
    srand(42);
    for (size_t i = 0; i < 512 && corrupt_offset + i < corrupted.size(); i++)
        corrupted[corrupt_offset + entropy_start + i] = rand() & 0xFF;

    const uint8_t *entropy = corrupted.data() + entropy_start;

    HuffCheckpoint state = {};
    if (tmpl.mcu_config.restart_interval > 0)
        state.mcus_to_restart = tmpl.mcu_config.restart_interval;

    HuffResult result = huff_validate_cluster(
        entropy, entropy_len,
        tmpl.mcu_config, tmpl.dc_tables, tmpl.ac_tables, state);

    printf("  Result: %s, %u MCUs before error, offset=%zu\n",
           result.passed ? "PASS (unexpected!)" : "FAIL (expected)",
           result.mcu_count, result.offset);

    if (!result.passed) {
        const char *err_names[] = {"OK","DC","AC","QA","EOF","RST","MARKER"};
        printf("  Error type: %s\n", err_names[result.error_type]);
        printf("  Corruption was at byte %zu, detected at %zu (delta=%zd)\n",
               corrupt_offset, result.offset,
               (ssize_t)result.offset - (ssize_t)corrupt_offset);
        return true; /* correctly detected corruption */
    }
    return false;
}

static bool test_appended_random(const char *path)
{
    printf("\nTest 3: Detect fragmentation point (append random data): %s\n", path);

    auto data = read_file(path);
    if (data.empty()) return false;

    JpegTemplate tmpl;
    if (!jpeg_parse_header(data.data(), data.size(), tmpl)) return false;

    /* Take first 4KB of entropy data, append 8KB of random data */
    size_t prefix = 4096;
    size_t random_size = 8192;
    size_t entropy_start = tmpl.header_bytes.size();

    if (entropy_start + prefix > data.size()) {
        printf("  SKIP: file too small for this test\n");
        return true;
    }

    std::vector<uint8_t> test_data(prefix + random_size);
    memcpy(test_data.data(), data.data() + entropy_start, prefix);
    srand(12345);
    for (size_t i = 0; i < random_size; i++)
        test_data[prefix + i] = rand() & 0xFF;

    HuffCheckpoint state = {};
    if (tmpl.mcu_config.restart_interval > 0)
        state.mcus_to_restart = tmpl.mcu_config.restart_interval;

    HuffResult result = huff_validate_cluster(
        test_data.data(), test_data.size(),
        tmpl.mcu_config, tmpl.dc_tables, tmpl.ac_tables, state);

    printf("  Result: %s, %u MCUs validated\n",
           result.passed ? "PASS" : "FAIL", result.mcu_count);

    if (!result.passed) {
        const char *err_names[] = {"OK","DC","AC","QA","EOF","RST","MARKER"};
        printf("  Error type: %s at offset %zu\n", err_names[result.error_type], result.offset);
        printf("  Prefix was %zu bytes, detection at %zu (delta=%zd)\n",
               prefix, result.offset, (ssize_t)result.offset - (ssize_t)prefix);

        bool close = (result.offset >= prefix - 16 && result.offset <= prefix + 64);
        printf("  %s: detection %s fragmentation point\n",
               close ? "GOOD" : "WARN",
               close ? "near" : "far from");
        return true;
    }

    printf("  UNEXPECTED: random data passed validation\n");
    return false;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: test_huffman <jpeg_file> [jpeg_file2...]\n");
        fprintf(stderr, "  Tests the Huffman validator against real JPEG files.\n");
        return 1;
    }

    int pass = 0, fail = 0;

    for (int i = 1; i < argc; i++) {
        printf("=== Testing: %s ===\n\n", argv[i]);

        if (test_good_jpeg(argv[i]))    pass++; else fail++;
        if (test_corrupt_jpeg(argv[i])) pass++; else fail++;
        if (test_appended_random(argv[i])) pass++; else fail++;

        printf("\n");
    }

    printf("=== Results: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
