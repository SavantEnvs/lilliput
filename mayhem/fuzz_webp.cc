// WebP decode (+ first-frame re-encode) through lilliput's libwebp wrapper, following
// webpDecoder.DecodeTo and webpEncoder.Encode in webp.go.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "common.h"
#include "webp.hpp"

namespace {
constexpr int kMaxFrames = 16;
constexpr size_t kIccBufferBytes = 64 * 1024;  // ICCProfileBufferSize
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return -1;  // the Go layer indexes buf[0]

    mh::Source src(data, size);
    if (!src.mat) return -1;

    webp_decoder dec = webp_decoder_create(src.mat);
    if (!dec) return -1;

    mh::Framebuffer fb;
    uint8_t* out = static_cast<uint8_t*>(malloc(mh::kEncodeBufferBytes));
    uint8_t* icc = static_cast<uint8_t*>(malloc(kIccBufferBytes));

    const int width = webp_decoder_get_width(dec);
    const int height = webp_decoder_get_height(dec);
    const int pixel_type = webp_decoder_get_pixel_type(dec);
    (void)webp_decoder_get_num_frames(dec);
    (void)webp_decoder_get_total_duration(dec);
    (void)webp_decoder_get_bg_color(dec);
    const uint32_t loop_count = webp_decoder_get_loop_count(dec);
    const size_t icc_len = webp_decoder_get_icc(dec, icc, kIccBufferBytes);

    bool encoded = false;
    webp_encoder enc = nullptr;

    for (int i = 0; i < kMaxFrames; i++) {
        if (!fb.resize(width, height, pixel_type)) break;  // ErrBufTooSmall
        // webpDecoder.DecodeTo: a failed decode ends the loop (io.EOF once no frames remain).
        if (!webp_decoder_decode(dec, fb.mat)) {
            (void)webp_decoder_has_more_frames(dec);
            break;
        }

        const int delay = webp_decoder_get_prev_frame_delay(dec);
        const int blend = webp_decoder_get_prev_frame_blend(dec);
        const int dispose = webp_decoder_get_prev_frame_dispose(dec);
        (void)webp_decoder_get_prev_frame_x_offset(dec);
        (void)webp_decoder_get_prev_frame_y_offset(dec);

        if (!encoded) {
            // Re-encode the first frame with the metadata the decoder reported, as
            // newWebpEncoder does (ICC passthrough, background colour, loop count).
            enc = webp_encoder_create(out, mh::kEncodeBufferBytes, icc, icc_len,
                                      webp_decoder_get_bg_color(dec), static_cast<int>(loop_count));
            if (enc) {
                const int opts[] = {CV_IMWRITE_WEBP_QUALITY, 80, WEBP_METHOD, 0};
                (void)webp_encoder_write(enc, fb.mat, opts, 4, delay, blend, dispose, 0, 0);
                (void)webp_encoder_flush(enc);
            }
            encoded = true;
        }

        webp_decoder_advance_frame(dec);
    }

    if (enc) webp_encoder_release(enc);
    webp_decoder_release(dec);
    free(icc);
    free(out);
    return 0;
}
