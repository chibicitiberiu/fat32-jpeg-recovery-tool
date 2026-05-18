/*
 * rst_recovery.cpp - Restart marker aware recovery
 *
 * When Huffman validation fails mid-cluster for a file with restart markers,
 * scan forward to the next RST marker and resume decoding. This skips the
 * corrupted region but recovers everything after it.
 *
 * Samsung Galaxy phones and some Nikon DSLRs include restart markers.
 * Without RST: single corruption cascades through entire remaining image.
 * With RST: corruption is contained between two markers, rest is recoverable.
 */
#include "sdrecov.h"
#include <cstring>

/*
 * Scan data for the next restart marker (FFD0-FFD7).
 * Returns offset of the FF byte, or -1 if not found.
 */
static ssize_t find_next_rst(const uint8_t *data, size_t len, size_t start)
{
    for (size_t i = start; i + 1 < len; i++) {
        if (data[i] == 0xFF && data[i + 1] >= 0xD0 && data[i + 1] <= 0xD7)
            return (ssize_t)i;
    }
    return -1;
}

/*
 * Try to recover past corruption by scanning for restart markers.
 *
 * When huff_validate_cluster fails at some offset, scan forward from that
 * point looking for RST markers. When found, reset DC predictors and
 * resume validation. This recovers the valid portions between RST markers.
 *
 * Returns the number of additional MCUs recovered past the corruption point.
 * Updates 'state' to the position after the last successfully decoded segment.
 */
uint32_t rst_skip_and_resume(const uint8_t *data, size_t len,
                              const McuConfig &cfg,
                              const HuffTable dc_tables[],
                              const HuffTable ac_tables[],
                              HuffCheckpoint &state,
                              size_t error_offset)
{
    if (cfg.restart_interval == 0)
        return 0; /* no restart markers in this file */

    uint32_t recovered_mcus = 0;
    size_t search_pos = error_offset;

    /* Try up to 10 restart marker hops */
    for (int hop = 0; hop < 10; hop++) {
        ssize_t rst_pos = find_next_rst(data, len, search_pos);
        if (rst_pos < 0) break;

        /* Found RST marker at rst_pos. Resume after it. */
        int rst_num = data[rst_pos + 1] - 0xD0;
        size_t resume_pos = rst_pos + 2;

        if (resume_pos >= len) break;

        /* Reset state for fresh decode from this RST point */
        HuffCheckpoint resume_state = {};
        resume_state.byte_pos = resume_pos;
        resume_state.mcu_count = state.mcu_count; /* approximate */
        resume_state.mcus_to_restart = cfg.restart_interval;
        resume_state.rst_counter = rst_num + 1;
        /* DC predictors are reset to 0 at restart markers */

        /* Try to decode from this point */
        HuffResult result = huff_validate_cluster(
            data + resume_pos, len - resume_pos,
            cfg, dc_tables, ac_tables, resume_state);

        uint32_t new_mcus = resume_state.mcu_count - state.mcu_count;

        if (new_mcus > 0) {
            recovered_mcus += new_mcus;
            state = resume_state;

            if (result.passed) {
                /* Decoded to end of cluster or all MCUs done */
                break;
            }
            /* Hit another error - try next RST marker */
            search_pos = resume_pos + result.offset;
        } else {
            /* No MCUs decoded from this RST - try next one */
            search_pos = resume_pos + 1;
        }
    }

    return recovered_mcus;
}
