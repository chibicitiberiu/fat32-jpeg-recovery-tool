/*
 * profiles.cpp - Preset corruption parameter sets.
 */
#include "corrsim.h"

struct ProfileDef {
    const char *name;
    const char *description;
    double ber;
    double bias;
    double degraded_frac;
    double swap_frac;
    double zero_frac;
    double wrong_data_frac;
    double fat_break_frac;
    double dir_corrupt_frac;
    int    fat_desync_entries;
    int    deleted_marks;
    double pre_corrupt_frac;
    bool   fragment;
    int    frag_count;
    double frag_delete_frac;
    /* JPEG-targeted corruption */
    double jpeg_dht_prob;
    double jpeg_dqt_prob;
    double jpeg_sof_prob;
    double jpeg_stuffing_prob;
    int    jpeg_stuffing_max;
    double jpeg_rst_prob;
    double jpeg_partial_zero_prob;
    double jpeg_misalign_prob;
    double jpeg_sos_prob;
};

static const ProfileDef profiles[] = {
    /* name, description,
     * ber, bias, degraded,
     * swap, zero, wrong_data,
     * fat_break, dir_corrupt, fat_desync, deleted_marks,
     * pre_corrupt, fragment, frag_count, frag_delete,
     * jpeg: dht, dqt, sof, stuffing, stuff_max, rst, partial_zero, misalign, sos */
    {
        "light", "Few bad sectors, mostly readable",
        1e-6, 0.80, 0.03,
        0.002, 0.001, 0.001,
        0.001, 0.02, 5, 2,
        0.0, false, 0, 0.0,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "moderate", "Noticeable damage, most files recoverable",
        1e-5, 0.80, 0.10,
        0.01, 0.005, 0.005,
        0.005, 0.05, 10, 5,
        0.2, true, 300, 0.50,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "heavy", "Serious damage, challenging recovery",
        5e-5, 0.80, 0.15,
        0.02, 0.01, 0.01,
        0.01, 0.10, 15, 10,
        0.4, true, 600, 0.65,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "catastrophic", "Near-death card, heroic recovery needed",
        1e-4, 0.80, 0.25,
        0.05, 0.03, 0.02,
        0.03, 0.20, 25, 20,
        0.5, true, 1000, 0.75,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "metadata-only", "File data intact, filesystem wrecked",
        0, 0.80, 0,
        0, 0, 0,
        0.02, 0.15, 20, 15,
        0.0, false, 0, 0.0,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "bitrot-only", "Pure NAND degradation, no FTL or metadata damage",
        5e-5, 0.85, 0.20,
        0, 0, 0,
        0, 0, 0, 0,
        0.3, false, 0, 0.0,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "real-sd", "Matches typical badly damaged SD card",
        3e-5, 0.80, 0.12,
        0.015, 0.008, 0.007,
        0.008, 0.08, 12, 8,
        0.3, true, 500, 0.60,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "fragmented", "Heavy fragmentation, no data corruption",
        0, 0.80, 0,
        0, 0, 0,
        0, 0, 0, 0,
        0.0, true, 1000, 0.80,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "header-targeted", "JPEG header corruption (DHT/DQT/SOF bit errors)",
        0, 0.80, 0,
        0, 0, 0,
        0.001, 0.02, 5, 2,
        0.0, false, 0, 0.0,
        0.30, 0.20, 0.10, 0, 0, 0, 0, 0, 0
    },
    {
        "accidental-markers", "Byte-stuffing corruption creates fake markers",
        1e-6, 0.80, 0.03,
        0, 0, 0,
        0.001, 0.02, 5, 2,
        0.0, false, 0, 0.0,
        0, 0, 0, 0.50, 5, 0, 0, 0, 0
    },
    {
        "rst-targeted", "RST marker corruption for DRI files",
        1e-6, 0.80, 0.03,
        0, 0, 0,
        0.001, 0.02, 5, 2,
        0.0, false, 0, 0.0,
        0, 0, 0, 0, 0, 0.40, 0, 0, 0
    },
    {
        "partial-zero", "Partial cluster zeroing (first/last 512B)",
        0, 0.80, 0,
        0, 0, 0,
        0.005, 0.05, 10, 5,
        0.0, false, 0, 0.0,
        0, 0, 0, 0, 0, 0, 0.30, 0, 0
    },
    {
        "seam-test", "Fragmented files with broken FAT chains (seam detection)",
        0, 0.80, 0,
        0, 0, 0,
        0.03, 0.02, 15, 3,
        0.0, true, 800, 0.70,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "deleted-only", "Most dir entries deleted, data and FAT intact",
        0, 0.80, 0,
        0, 0, 0,
        0, 0.90, 0, 999,
        0.0, false, 0, 0.0,
        0, 0, 0, 0, 0, 0, 0, 0, 0
    },
    {
        "misaligned", "JPEG data shifted within first cluster (mid-cluster SOI)",
        0, 0.80, 0,
        0, 0, 0,
        0, 0.02, 5, 2,
        0.0, false, 0, 0.0,
        0, 0, 0, 0, 0, 0, 0, 0.40, 0
    },
    {
        "sos-corrupt", "Corrupted SOS table indices (Td/Ta out of range)",
        1e-6, 0.80, 0.03,
        0, 0, 0,
        0.001, 0.02, 5, 2,
        0.0, false, 0, 0.0,
        0, 0, 0, 0, 0, 0, 0, 0, 0.40
    },
};

static constexpr int NUM_PROFILES = sizeof(profiles) / sizeof(profiles[0]);

bool apply_profile(const std::string &name, SimConfig &cfg)
{
    for (int i = 0; i < NUM_PROFILES; i++) {
        if (name == profiles[i].name) {
            auto &p = profiles[i];
            cfg.bitflip.ber           = p.ber;
            cfg.bitflip.bias_0to1     = p.bias;
            cfg.bitflip.degraded_frac = p.degraded_frac;
            cfg.ftl.swap_frac         = p.swap_frac;
            cfg.ftl.zero_frac         = p.zero_frac;
            cfg.ftl.wrong_data_frac   = p.wrong_data_frac;
            cfg.metadata.fat_chain_break_frac  = p.fat_break_frac;
            cfg.metadata.dir_entry_corrupt_frac = p.dir_corrupt_frac;
            cfg.metadata.fat_desync_entries     = p.fat_desync_entries;
            cfg.metadata.deleted_marks          = p.deleted_marks;

            cfg.pre_corrupt_frac = p.pre_corrupt_frac;
            if (p.fragment) {
                cfg.fragment.enabled    = true;
                cfg.fragment.file_count = p.frag_count;
                cfg.fragment.delete_frac = p.frag_delete_frac;
            }

            // JPEG-targeted corruption
            cfg.jpeg.dht_corrupt_prob      = p.jpeg_dht_prob;
            cfg.jpeg.dqt_corrupt_prob      = p.jpeg_dqt_prob;
            cfg.jpeg.sof_corrupt_prob      = p.jpeg_sof_prob;
            cfg.jpeg.stuffing_corrupt_prob = p.jpeg_stuffing_prob;
            cfg.jpeg.stuffing_max_flips    = p.jpeg_stuffing_max;
            cfg.jpeg.rst_corrupt_prob      = p.jpeg_rst_prob;
            cfg.jpeg.partial_zero_prob     = p.jpeg_partial_zero_prob;
            cfg.jpeg.misalign_prob         = p.jpeg_misalign_prob;
            cfg.jpeg.sos_corrupt_prob      = p.jpeg_sos_prob;

            // Set passes based on what's non-zero
            cfg.passes = 0;
            if (p.ber > 0) cfg.passes |= PASS_BITFLIP;
            if (p.swap_frac > 0 || p.zero_frac > 0 || p.wrong_data_frac > 0)
                cfg.passes |= PASS_FTL;
            if (p.fat_break_frac > 0 || p.dir_corrupt_frac > 0)
                cfg.passes |= PASS_META;
            if (p.jpeg_dht_prob > 0 || p.jpeg_dqt_prob > 0 ||
                p.jpeg_sof_prob > 0 || p.jpeg_stuffing_prob > 0 ||
                p.jpeg_rst_prob > 0 || p.jpeg_partial_zero_prob > 0 ||
                p.jpeg_misalign_prob > 0 || p.jpeg_sos_prob > 0)
                cfg.passes |= PASS_JPEG;
            return true;
        }
    }
    return false;
}

void list_profiles()
{
    fprintf(stderr, "Available corruption profiles:\n");
    for (int i = 0; i < NUM_PROFILES; i++) {
        fprintf(stderr, "  %-16s %s\n", profiles[i].name, profiles[i].description);
        fprintf(stderr, "                   BER=%.0e  degraded=%.0f%%  swap=%.1f%%  "
                "zero=%.1f%%  fat_break=%.1f%%  dir_corrupt=%.0f%%\n",
                profiles[i].ber,
                profiles[i].degraded_frac * 100,
                profiles[i].swap_frac * 100,
                profiles[i].zero_frac * 100,
                profiles[i].fat_break_frac * 100,
                profiles[i].dir_corrupt_frac * 100);
        fprintf(stderr, "                   pre_corrupt=%.0f%%  fragment=%s",
                profiles[i].pre_corrupt_frac * 100,
                profiles[i].fragment ? "yes" : "no");
        if (profiles[i].fragment)
            fprintf(stderr, " (count=%d, delete=%.0f%%)",
                    profiles[i].frag_count,
                    profiles[i].frag_delete_frac * 100);
        fprintf(stderr, "\n");
    }
}
