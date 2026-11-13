// Known-answer probe for mayhem/test.sh. Decodes the repo's own fixtures through the same C wrappers
// the fuzz targets drive and prints computed values (dimensions, frame counts, pixel hashes, codec
// names, re-encode round trips). It is built with normal flags (no sanitizers) and linked
// dynamically, so a program that exits early prints nothing and test.sh fails.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avcodec.hpp"
#include "avif.hpp"
#include "common.h"
#include "giflib.hpp"
#include "opencv.hpp"
#include "webp.hpp"

namespace {

uint8_t* read_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path);
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = static_cast<uint8_t*>(malloc(n > 0 ? n : 1));
    if (fread(buf, 1, n, f) != static_cast<size_t>(n)) {
        fclose(f);
        free(buf);
        return nullptr;
    }
    fclose(f);
    *size = static_cast<size_t>(n);
    return buf;
}

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

uint64_t mat_hash(opencv_mat m, int channels) {
    return fnv1a(static_cast<const uint8_t*>(opencv_mat_get_data(m)),
                 size_t(opencv_mat_get_width(m)) * opencv_mat_get_height(m) * channels);
}

int probe_gif(const uint8_t* data, size_t size) {
    mh::Source src(data, size);
    giflib_decoder dec = giflib_decoder_create(src.mat);
    if (!dec) return 1;
    const int w = giflib_decoder_get_width(dec);
    const int h = giflib_decoder_get_height(dec);
    const GifAnimationInfo info = giflib_decoder_get_animation_info(dec);
    printf("gif size=%dx%d frames=%d loop=%d duration_ms=%d bg=%d,%d,%d,%d\n", w, h, info.frame_count,
           info.loop_count, info.duration_ms, info.bg_red, info.bg_green, info.bg_blue, info.bg_alpha);

    mh::Framebuffer fb;
    uint8_t* out = static_cast<uint8_t*>(malloc(mh::kEncodeBufferBytes));
    giflib_encoder enc = giflib_encoder_create(out, mh::kEncodeBufferBytes);
    int decoded = 0, delay_sum = 0;
    uint64_t first_hash = 0, last_hash = 0;
    while (true) {
        if (!fb.resize(w, h, CV_8UC4)) return 1;
        if (giflib_decoder_decode_frame_header(dec) != giflib_decoder_have_next_frame) break;
        if (!giflib_decoder_decode_frame(dec, fb.mat)) return 1;
        last_hash = mat_hash(fb.mat, 4);
        if (decoded == 0) first_hash = last_hash;
        delay_sum += giflib_decoder_get_prev_frame_delay(dec);
        if (decoded == 0) giflib_encoder_init(enc, dec, w, h);
        if (!giflib_encoder_encode_frame(enc, dec, fb.mat)) return 1;
        decoded++;
    }
    printf("gif decoded=%d delay_cs=%d first=%016llx last=%016llx\n", decoded, delay_sum,
           (unsigned long long)first_hash, (unsigned long long)last_hash);

    if (!giflib_encoder_flush(enc, dec)) return 1;
    const int out_len = giflib_encoder_get_output_length(enc);
    giflib_encoder_release(enc);
    giflib_decoder_release(dec);

    mh::Source re(out, out_len);
    giflib_decoder dec2 = giflib_decoder_create(re.mat);
    if (!dec2) return 1;
    const GifAnimationInfo info2 = giflib_decoder_get_animation_info(dec2);
    printf("gif reencode size=%dx%d frames=%d\n", giflib_decoder_get_width(dec2),
           giflib_decoder_get_height(dec2), info2.frame_count);
    giflib_decoder_release(dec2);
    free(out);
    return 0;
}

int probe_webp(const uint8_t* data, size_t size) {
    mh::Source src(data, size);
    webp_decoder dec = webp_decoder_create(src.mat);
    if (!dec) return 1;
    const int w = webp_decoder_get_width(dec);
    const int h = webp_decoder_get_height(dec);
    const int type = webp_decoder_get_pixel_type(dec);
    printf("webp size=%dx%d channels=%d frames=%d loop=%u duration_ms=%d\n", w, h,
           opencv_type_channels(type), webp_decoder_get_num_frames(dec), webp_decoder_get_loop_count(dec),
           webp_decoder_get_total_duration(dec));

    mh::Framebuffer fb;
    uint8_t* out = static_cast<uint8_t*>(malloc(mh::kEncodeBufferBytes));
    int decoded = 0;
    uint64_t first_hash = 0;
    size_t encoded_len = 0;
    while (true) {
        if (!fb.resize(w, h, type)) return 1;
        if (!webp_decoder_decode(dec, fb.mat)) break;
        if (decoded == 0) {
            first_hash = mat_hash(fb.mat, opencv_type_channels(type));
            webp_encoder enc = webp_encoder_create(out, mh::kEncodeBufferBytes, nullptr, 0, 0, 0);
            const int opts[] = {CV_IMWRITE_WEBP_QUALITY, 80, WEBP_METHOD, 0};
            webp_encoder_write(enc, fb.mat, opts, 4, 0, 0, 0, 0, 0);
            encoded_len = webp_encoder_flush(enc);
            webp_encoder_release(enc);
        }
        decoded++;
        webp_decoder_advance_frame(dec);
    }
    printf("webp decoded=%d first=%016llx\n", decoded, (unsigned long long)first_hash);

    mh::Source re(out, encoded_len);
    webp_decoder dec2 = webp_decoder_create(re.mat);
    if (!dec2) return 1;
    printf("webp reencode size=%dx%d\n", webp_decoder_get_width(dec2), webp_decoder_get_height(dec2));
    webp_decoder_release(dec2);
    webp_decoder_release(dec);
    free(out);
    return 0;
}

int probe_avif(const uint8_t* data, size_t size) {
    mh::Source src(data, size);
    avif_decoder dec = avif_decoder_create(src.mat, false);
    if (!dec) return 1;
    const int w = avif_decoder_get_width(dec);
    const int h = avif_decoder_get_height(dec);
    const int type = avif_decoder_get_pixel_type(dec);
    printf("avif size=%dx%d channels=%d frames=%d orientation=%d\n", w, h, opencv_type_channels(type),
           avif_decoder_get_num_frames(dec), avif_decoder_get_orientation(dec));
    mh::Framebuffer fb;
    int decoded = 0;
    uint64_t first_hash = 0;
    while (avif_decoder_has_more_frames(dec)) {
        if (!fb.resize(w, h, type)) return 1;
        if (!avif_decoder_decode(dec, fb.mat)) return 1;
        if (decoded == 0) first_hash = mat_hash(fb.mat, opencv_type_channels(type));
        decoded++;
    }
    printf("avif decoded=%d first=%016llx\n", decoded, (unsigned long long)first_hash);
    avif_decoder_release(dec);
    return 0;
}

int probe_image(const uint8_t* data, size_t size) {
    mh::Source src(data, size);
    opencv_decoder dec = opencv_decoder_create(src.mat);
    if (!dec) return 1;
    if (!opencv_decoder_read_header(dec)) return 1;
    const int w = opencv_decoder_get_width(dec);
    const int h = opencv_decoder_get_height(dec);
    const int type = opencv_decoder_get_pixel_type(dec);
    printf("image format=%s size=%dx%d channels=%d depth=%d orientation=%d\n",
           opencv_decoder_get_description(dec), w, h, opencv_type_channels(type),
           opencv_type_depth(type), opencv_decoder_get_orientation(dec));
    mh::Framebuffer fb;
    if (!fb.resize(w, h, type)) return 1;
    if (!opencv_decoder_read_data(dec, fb.mat)) return 1;
    int out_type = opencv_type_depth(type) > 8 ? opencv_type_convert_depth(type, CV_8U) : type;
    printf("image pixels=%016llx\n", (unsigned long long)mat_hash(fb.mat, opencv_type_channels(out_type)));
    opencv_decoder_release(dec);
    return 0;
}

int probe_video(const uint8_t* data, size_t size) {
    avcodec_init();
    mh::Source src(data, size);
    avcodec_decoder dec = avcodec_decoder_create(src.mat, true, true);
    if (!dec) return 1;
    printf("video format=%s video=%s audio=%s size=%dx%d duration_ms=%d keyframes=%d\n",
           avcodec_decoder_get_description(dec), avcodec_decoder_get_video_codec(dec),
           avcodec_decoder_get_audio_codec(dec), avcodec_decoder_get_width(dec),
           avcodec_decoder_get_height(dec), static_cast<int>(avcodec_decoder_get_duration(dec) * 1000.0f + 0.5f),
           avcodec_decoder_get_keyframe_count(dec));
    mh::Framebuffer fb;
    if (!fb.resize(avcodec_decoder_get_width(dec), avcodec_decoder_get_height(dec), CV_8UC4)) return 1;
    printf("video decode=%d\n", avcodec_decoder_decode(dec, fb.mat) ? 1 : 0);
    avcodec_decoder_release(dec);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s gif|webp|avif|image|video <file>\n", argv[0]);
        return 2;
    }
    size_t size = 0;
    uint8_t* data = read_file(argv[2], &size);
    if (!data || size == 0) return 1;
    int rc = 2;
    if (!strcmp(argv[1], "gif")) rc = probe_gif(data, size);
    else if (!strcmp(argv[1], "webp")) rc = probe_webp(data, size);
    else if (!strcmp(argv[1], "avif")) rc = probe_avif(data, size);
    else if (!strcmp(argv[1], "image")) rc = probe_image(data, size);
    else if (!strcmp(argv[1], "video")) rc = probe_video(data, size);
    free(data);
    return rc;
}
