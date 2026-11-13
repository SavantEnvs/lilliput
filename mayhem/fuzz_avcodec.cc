// Video/audio container probe and first-frame decode through lilliput's FFmpeg wrapper,
// following newAVCodecDecoder / avCodecDecoder.DecodeTo in avcodec.go.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "common.h"
#include "avcodec.hpp"

namespace {
constexpr size_t kIccBufferBytes = 64 * 1024;  // ICCProfileBufferSize
constexpr int kMaxKeyframes = 64;
}  // namespace

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    avcodec_init();
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return -1;  // the Go layer indexes buf[0]

    mh::Source src(data, size);
    if (!src.mat) return -1;

    // newAVCodecDecoder probes streamability before creating the decoder.
    (void)avcodec_decoder_is_streamable(src.mat);

    // HEVC and AV1 are the build-time opt-in codecs (-X hevcEnabled / av1Enabled); enable both so
    // their demux/decode paths are reachable.
    avcodec_decoder dec = avcodec_decoder_create(src.mat, true, true);
    if (!dec) return -1;

    mh::Framebuffer fb;
    uint8_t* icc = static_cast<uint8_t*>(malloc(kIccBufferBytes));
    avcodec_keyframe_entry* keyframes =
        static_cast<avcodec_keyframe_entry*>(malloc(sizeof(avcodec_keyframe_entry) * kMaxKeyframes));

    (void)avcodec_decoder_has_subtitles(dec);
    (void)avcodec_decoder_get_description(dec);
    (void)avcodec_decoder_get_video_codec(dec);
    (void)avcodec_decoder_get_audio_codec(dec);
    (void)avcodec_decoder_get_duration(dec);
    (void)avcodec_decoder_get_orientation(dec);
    (void)avcodec_decoder_get_icc(dec, icc, kIccBufferBytes);
    (void)avcodec_decoder_get_keyframe_count(dec);
    (void)avcodec_decoder_get_keyframes(dec, keyframes, kMaxKeyframes);
    (void)avcodec_decoder_get_codec_id(dec);

    const int width = avcodec_decoder_get_width(dec);
    const int height = avcodec_decoder_get_height(dec);
    if (fb.resize(width, height, CV_8UC4)) {  // else ErrBufTooSmall
        (void)avcodec_decoder_decode(dec, fb.mat);
    }

    // DecodeRawKeyframe path: the embedder range-reads the keyframe chunk out of the file using the
    // index entry and hands it back with the codec id and extradata from the moov parse.
    const int nkeys = avcodec_decoder_get_keyframes(dec, keyframes, kMaxKeyframes);
    const int codec_id = avcodec_decoder_get_codec_id(dec);
    const int extradata_len = avcodec_decoder_get_extradata(dec, nullptr, 0);
    if (nkeys > 0 && codec_id >= 0 && extradata_len >= 0 && width > 0 && height > 0) {
        uint8_t* extradata = static_cast<uint8_t*>(malloc(extradata_len > 0 ? extradata_len : 1));
        const int copied = avcodec_decoder_get_extradata(dec, extradata, extradata_len);
        const avcodec_keyframe_entry& kf = keyframes[0];
        // bgraStrideBufSize: rows are padded to 32 pixels for the sws_scale path.
        const size_t padded_w = (width % 32) ? size_t(width) + 32 - (width % 32) : size_t(width);
        if (copied >= 0 && kf.byte_offset >= 0 && kf.size > 0 &&
            static_cast<uint64_t>(kf.byte_offset) + static_cast<uint64_t>(kf.size) <= size &&
            padded_w * size_t(height) * 4 <= mh::kFramebufferBytes) {
            // Exact-size heap copy of the chunk, as a range request would return.
            uint8_t* chunk = static_cast<uint8_t*>(malloc(kf.size));
            memcpy(chunk, data + kf.byte_offset, kf.size);
            mh::Framebuffer raw;
            if (raw.resize(width, height, CV_8UC4)) {
                (void)avcodec_decode_raw_keyframe(codec_id, extradata, copied, width, height, chunk,
                                                  kf.size, raw.mat);
            }
            free(chunk);
        }
        free(extradata);
    }

    avcodec_decoder_release(dec);
    free(keyframes);
    free(icc);
    return 0;
}
