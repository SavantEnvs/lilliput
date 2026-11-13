// GIF decode (+ re-encode) through lilliput's giflib wrapper, following gifDecoder.DecodeTo and
// gifEncoder.Encode in giflib.go.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "common.h"
#include "giflib.hpp"

namespace {

// giflib.go: defaultMaxFrameDimension, applied by DecodeTo after the frame header is read.
constexpr int kMaxFrameDimension = 10000;
constexpr int kMaxFrames = 24;

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return -1;  // the Go layer indexes buf[0]

    mh::Source src(data, size);
    if (!src.mat) return -1;

    giflib_decoder dec = giflib_decoder_create(src.mat);
    if (!dec) return -1;

    {
        mh::Framebuffer fb;
        uint8_t* out = static_cast<uint8_t*>(malloc(mh::kEncodeBufferBytes));
        giflib_encoder enc = giflib_encoder_create(out, mh::kEncodeBufferBytes);
        bool encoder_ready = false;

        (void)giflib_decoder_get_animation_info(dec);
        (void)giflib_decoder_get_num_frames(dec);

        const int width = giflib_decoder_get_width(dec);
        const int height = giflib_decoder_get_height(dec);

        for (int i = 0; i < kMaxFrames; i++) {
            if (!fb.resize(width, height, CV_8UC4)) break;  // ErrBufTooSmall

            giflib_decoder_frame_state st = giflib_decoder_decode_frame_header(dec);
            if (st != giflib_decoder_have_next_frame) break;

            const int fw = giflib_decoder_get_frame_width(dec);
            const int fh = giflib_decoder_get_frame_height(dec);
            if (fw > kMaxFrameDimension || fh > kMaxFrameDimension) break;

            if (!giflib_decoder_decode_frame(dec, fb.mat)) break;
            (void)giflib_decoder_get_prev_frame_delay(dec);
            (void)giflib_decoder_get_prev_frame_disposal(dec);

            if (enc) {
                if (!encoder_ready) {
                    giflib_encoder_init(enc, dec, width, height);
                    encoder_ready = true;
                }
                if (!giflib_encoder_encode_frame(enc, dec, fb.mat)) break;
            }
        }

        if (enc) {
            if (encoder_ready && giflib_encoder_flush(enc, dec)) {
                (void)giflib_encoder_get_output_length(enc);
            }
            giflib_encoder_release(enc);
        }
        free(out);
    }
    giflib_decoder_release(dec);

    // Second pass over the same bytes: the streaming SkipFrame path (gifDecoder.SkipFrame).
    giflib_decoder skipper = giflib_decoder_create(src.mat);
    if (skipper) {
        for (int i = 0; i < kMaxFrames; i++) {
            if (giflib_decoder_skip_frame(skipper) != giflib_decoder_have_next_frame) break;
        }
        giflib_decoder_release(skipper);
    }
    return 0;
}
