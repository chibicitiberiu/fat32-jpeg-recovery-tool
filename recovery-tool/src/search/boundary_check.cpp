/*
 * boundary_check.cpp - Pixel-level boundary coherence via first-MCU DC isolation
 *
 * The key anti-stitching signal: decode ONLY the first MCU of a candidate
 * cluster and compare its DC coefficients against the last MCU's DC values
 * from the current cluster. This gives an undiluted boundary signal.
 *
 * When the correct next cluster continues the same image, the DC values
 * change smoothly (small delta). When a wrong cluster from a different
 * photo is stitched, the DC values jump wildly (different scene).
 *
 * DC predictors in JPEG are cumulative: after decoding MCU N, dc_pred[c]
 * IS the reconstructed DC value for component c. After decoding MCU N+1
 * (first MCU of candidate), the DIFFERENCE dc_pred_new - dc_pred_old
 * gives the DC coefficient of that first MCU, which represents the
 * average brightness/color change from one MCU row to the next.
 */
#include "sdrecov.h"
#include <cmath>

/*
 * Check boundary coherence by decoding exactly 1 MCU of the candidate.
 *
 * pre_state: decoder state at end of current cluster (dc_pred = last MCU's DC values)
 * data/len: candidate cluster data
 *
 * Returns 0.0 (scene change / wrong cluster) to 1.0 (smooth continuation).
 * Returns -1.0 if the MCU can't be decoded (data too short, Huffman error).
 */
float first_mcu_boundary_check(const uint8_t *data, size_t len,
                                const McuConfig &cfg,
                                const HuffTable dc_tables[],
                                const HuffTable ac_tables[],
                                const HuffCheckpoint &pre_state)
{
    /* For progressive non-DC scans, DC predictor comparison is not meaningful */
    if (cfg.jpeg_mode == JPEG_PROGRESSIVE && pre_state.scan_ss > 0)
        return -1.0f; /* skip boundary check */

    /* Copy state - we don't want to modify the original */
    HuffCheckpoint test_state = pre_state;
    test_state.byte_pos = 0; /* reading from start of candidate cluster */

    /* Save DC predictors before the MCU */
    int32_t dc_before[MAX_COMPONENTS];
    for (int i = 0; i < MAX_COMPONENTS; i++)
        dc_before[i] = test_state.dc_pred[i];

    /* Decode exactly 1 MCU */
    HuffResult r = huff_validate_one_mcu(data, len, cfg, dc_tables, ac_tables, test_state);

    if (!r.passed) {
        /* INVALID code = definitely wrong cluster */
        if (r.error_type == HUFF_ERR_DC || r.error_type == HUFF_ERR_AC ||
            r.error_type == HUFF_ERR_QA)
            return 0.0f;
        /* EOF or MARKER = can't decode enough data, skip boundary check */
        return -1.0f;
    }

    /* Compare DC predictors before and after this single MCU.
     *
     * dc_before[c] = DC value at end of previous cluster (component c)
     * test_state.dc_pred[c] = DC value after first MCU of candidate
     * delta = test_state.dc_pred[c] - dc_before[c] = DC coefficient of first MCU
     *
     * For the SAME image continuing:
     *   - Y (luminance) delta is typically small (-100..+100 for smooth scenes)
     *   - Cb/Cr (chrominance) deltas are very small (same color temperature)
     *
     * For a DIFFERENT image stitched in:
     *   - Y delta can be huge (-1000..+1000, completely different brightness)
     *   - Cb/Cr deltas are also large (different white balance / scene colors)
     *
     * The chrominance channels are actually MORE discriminative because they
     * change less within a single scene but a lot between different scenes.
     */

    float y_delta = 0, cb_delta = 0, cr_delta = 0;
    int n_comp = 0;

    for (int c = 0; c < MAX_COMPONENTS; c++) {
        float delta = std::abs((float)(test_state.dc_pred[c] - dc_before[c]));
        if (c == 0) y_delta = delta;      /* Y = luminance */
        else if (c == 1) cb_delta = delta; /* Cb = blue chroma */
        else if (c == 2) cr_delta = delta; /* Cr = red chroma */
        if (dc_before[c] != 0 || test_state.dc_pred[c] != 0) n_comp++;
    }

    if (n_comp == 0) return -1.0f;

    /*
     * Score based on DC deltas.
     *
     * For boundary between MCU rows in the SAME image:
     *   Y delta: 0-50 (gradual brightness change)
     *   Cb/Cr delta: 0-20 (color stays consistent)
     *
     * For boundary between DIFFERENT images:
     *   Y delta: 50-2000 (completely different scene)
     *   Cb/Cr delta: 20-500 (different color balance)
     *
     * We combine Y and chroma signals, with chroma weighted more heavily
     * because it's more discriminative (same-scene chroma is very stable).
     */

    float chroma_delta = (cb_delta + cr_delta) / 2.0f;

    /* Weighted combined score: chroma counts 2x because it's more discriminative */
    float combined = y_delta + chroma_delta * 2.0f;

    /* Convert to 0..1 coherence score */
    if (combined < 30.0f)  return 1.0f;   /* very smooth - almost certainly correct */
    if (combined < 80.0f)  return 0.9f;   /* smooth - likely correct */
    if (combined < 150.0f) return 0.7f;   /* moderate change - possibly scene transition within same photo */
    if (combined < 300.0f) return 0.4f;   /* suspicious - likely different photo */
    if (combined < 600.0f) return 0.1f;   /* very suspicious */
    return 0.0f;                           /* almost certainly wrong cluster */
}

/*
 * Original aggregate boundary coherence (kept for backward compat).
 */
float boundary_coherence(const HuffCheckpoint &pre_state,
                          const HuffCheckpoint &post_state,
                          uint32_t new_mcus)
{
    if (new_mcus == 0) return 0.0f;

    float max_jump = 0;
    for (int c = 0; c < MAX_COMPONENTS; c++) {
        float jump = std::abs((float)(post_state.dc_pred[c] - pre_state.dc_pred[c]));
        if (jump > max_jump) max_jump = jump;
    }

    float boundary_estimate = max_jump / std::sqrt((float)new_mcus);

    if (boundary_estimate < 20.0f)  return 1.0f;
    if (boundary_estimate < 50.0f)  return 0.8f;
    if (boundary_estimate < 100.0f) return 0.5f;
    if (boundary_estimate < 200.0f) return 0.2f;
    return 0.0f;
}
