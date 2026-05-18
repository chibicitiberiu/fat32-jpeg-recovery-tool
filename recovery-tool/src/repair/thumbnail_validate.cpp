/*
 * thumbnail_validate.cpp - Chain-level validation using EXIF thumbnails
 *
 * After building a chain, compare the recovered image against the embedded
 * EXIF thumbnail. If the thumbnail was in the first cluster (usually is),
 * it survived corruption even when later clusters are wrong.
 *
 * Uses libjpeg-turbo for decoding both the thumbnail and the recovered file,
 * then computes Mean Absolute Error (MAE) between downscaled versions.
 *
 * For now: a simpler approach that doesn't require libjpeg linkage.
 * Compare raw bytes of the first few KB after the header between the
 * recovered file and what we'd expect from the thumbnail's content.
 *
 * Actually, the most practical approach without adding libjpeg as a build
 * dependency: use a perceptual hash. But even simpler: just compare the
 * first N bytes of entropy data between the thumbnail and the main image.
 * If the main image starts with the same DCT coefficients (same scene),
 * the first entropy bytes will be similar.
 *
 * Simplest useful approach: decode the thumbnail with our own Huffman
 * validator to get DC predictor values, then compare against the main
 * image's DC values. Same scene = similar DC baselines.
 */
#include "sdrecov.h"
#include <cstring>
#include <cmath>

/*
 * Validate a recovered chain against its EXIF thumbnail.
 *
 * Strategy: parse the thumbnail JPEG, decode a few MCUs to get DC predictor
 * baselines. Parse the recovered image, decode a few MCUs. Compare the DC
 * values - if they're from the same scene, the Y (luminance) DC values
 * should be correlated.
 *
 * Returns a confidence score 0.0 (definitely wrong) to 1.0 (definitely right).
 * Returns -1.0 if validation is not possible (no thumbnail, parse failure, etc.)
 */
float thumbnail_validate(const RecoveryContext &ctx, const Seed &seed,
                          const uint8_t *recovered_data, size_t recovered_len)
{
    if (!seed.has_thumbnail || seed.thumbnail_size < 100)
        return -1.0f;

    /* The thumbnail is embedded in the EXIF header, which is part of
     * the recovered data. Use recovered_data directly to avoid buffer
     * overread when the thumbnail spans multiple clusters. */
    if (seed.thumbnail_offset + seed.thumbnail_size > recovered_len)
        return -1.0f;

    const uint8_t *thumb_data = recovered_data + seed.thumbnail_offset;
    uint32_t thumb_len = seed.thumbnail_size;

    /* Verify the thumbnail starts with SOI */
    if (thumb_len < 4 || thumb_data[0] != 0xFF || thumb_data[1] != 0xD8)
        return -1.0f;

    /* Parse thumbnail JPEG header */
    JpegTemplate thumb_tmpl;
    if (!jpeg_parse_header(thumb_data, thumb_len, thumb_tmpl))
        return -1.0f;

    /* Decode a few MCUs from the thumbnail */
    auto &tcfg = thumb_tmpl.mcu_config;
    size_t thumb_entropy_off = thumb_tmpl.header_bytes.size();
    if (thumb_entropy_off >= thumb_len) return -1.0f;

    HuffCheckpoint thumb_state = {};
    if (tcfg.restart_interval > 0)
        thumb_state.mcus_to_restart = tcfg.restart_interval;

    HuffResult thumb_result = huff_validate_cluster(
        thumb_data + thumb_entropy_off,
        thumb_len - thumb_entropy_off,
        tcfg, thumb_tmpl.dc_tables, thumb_tmpl.ac_tables, thumb_state);

    if (thumb_state.mcu_count < 3) return -1.0f;

    /* Parse recovered image header */
    JpegTemplate main_tmpl;
    if (!jpeg_parse_header(recovered_data, recovered_len, main_tmpl))
        return -1.0f;

    auto &mcfg = main_tmpl.mcu_config;
    size_t main_entropy_off = main_tmpl.header_bytes.size();
    if (main_entropy_off >= recovered_len) return -1.0f;

    HuffCheckpoint main_state = {};
    if (mcfg.restart_interval > 0)
        main_state.mcus_to_restart = mcfg.restart_interval;

    /* Decode just a few MCUs from the main image */
    size_t decode_len = std::min(recovered_len - main_entropy_off, (size_t)8192);
    HuffResult main_result = huff_validate_cluster(
        recovered_data + main_entropy_off, decode_len,
        mcfg, main_tmpl.dc_tables, main_tmpl.ac_tables, main_state);

    if (main_state.mcu_count < 3) return -1.0f;

    /*
     * Compare DC predictor values.
     * The thumbnail and main image encode the same scene at different
     * resolutions. The DC values represent average brightness/color per
     * block. For the same scene:
     *   - Y (luminance) DC values should have similar magnitude
     *   - Cb/Cr (chrominance) DC values should be near zero for both
     *
     * We compare the final DC predictor values after decoding a few MCUs.
     * This is rough but catches gross mismatches (wrong image entirely).
     */
    float total_diff = 0;
    int components = 0;
    for (int c = 0; c < MAX_COMPONENTS; c++) {
        if (thumb_state.dc_pred[c] == 0 && main_state.dc_pred[c] == 0)
            continue;
        float diff = std::abs((float)thumb_state.dc_pred[c] - (float)main_state.dc_pred[c]);
        /* Normalize by the magnitude of the values */
        float mag = std::max(std::abs((float)thumb_state.dc_pred[c]),
                              std::abs((float)main_state.dc_pred[c]));
        if (mag > 1.0f)
            total_diff += diff / mag;
        else
            total_diff += diff;
        components++;
    }

    if (components == 0) return -1.0f;
    float avg_diff = total_diff / components;

    /* Convert to confidence: low diff = high confidence */
    float confidence;
    if (avg_diff < 0.3f)       confidence = 1.0f;
    else if (avg_diff < 0.6f)  confidence = 0.8f;
    else if (avg_diff < 1.0f)  confidence = 0.5f;
    else if (avg_diff < 2.0f)  confidence = 0.2f;
    else                        confidence = 0.0f;

    log_debug(ctx, "thumbnail validate: avg_diff=%.2f, confidence=%.2f (%d components)",
              avg_diff, confidence, components);

    return confidence;
}
