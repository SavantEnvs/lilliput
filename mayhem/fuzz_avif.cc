// AVIF decode through lilliput's libavif wrapper, following avifDecoder.DecodeTo in avif.go.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "common.h"
#include "avif.hpp"

namespace {
constexpr int kMaxFrames = 8;
constexpr size_t kIccBufferBytes = 64 * 1024;  // ICCProfileBufferSize
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return -1;  // the Go layer indexes buf[0]

    mh::Source src(data, size);
    if (!src.mat) return -1;

    // The tone-mapping switch is a per-decoder option the embedder sets; derive it from the input so
    // both settings are reached and each input still has one deterministic result.
    const bool tone_mapping = (size & 1) != 0;
    avif_decoder dec = avif_decoder_create(src.mat, tone_mapping);
    if (!dec) return -1;

    mh::Framebuffer fb;
    uint8_t* icc = static_cast<uint8_t*>(malloc(kIccBufferBytes));

    (void)avif_decoder_get_num_frames(dec);
    (void)avif_decoder_get_orientation(dec);
    (void)avif_decoder_get_duration(dec);
    (void)avif_decoder_get_loop_count(dec);
    (void)avif_decoder_get_bg_color(dec);
    (void)avif_decoder_get_total_duration(dec);
    (void)avif_decoder_get_icc(dec, icc, kIccBufferBytes);

    const int width = avif_decoder_get_width(dec);
    const int height = avif_decoder_get_height(dec);
    const int pixel_type = avif_decoder_get_pixel_type(dec);

    for (int i = 0; i < kMaxFrames; i++) {
        if (avif_decoder_has_more_frames(dec) == 0) break;
        if (!fb.resize(width, height, pixel_type)) break;  // ErrBufTooSmall
        if (!avif_decoder_decode(dec, fb.mat)) break;
        (void)avif_decoder_get_frame_duration(dec);
        (void)avif_decoder_get_frame_dispose(dec);
        (void)avif_decoder_get_frame_blend(dec);
        (void)avif_decoder_get_frame_x_offset(dec);
        (void)avif_decoder_get_frame_y_offset(dec);
    }

    avif_decoder_release(dec);
    free(icc);
    return 0;
}
