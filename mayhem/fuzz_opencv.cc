// Still-image decode (JPEG, PNG, ...) through lilliput's OpenCV wrapper, then the transform and
// encode steps an embedder chains after it (ops.go): orientation, resize, re-encode, cICP insert.
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "color_info.hpp"
#include "opencv.hpp"

namespace {

constexpr size_t kIccBufferBytes = 64 * 1024;  // ICCProfileBufferSize
constexpr int kThumbEdge = 64;
constexpr size_t kThumbBytes = size_t(kThumbEdge) * kThumbEdge * 4;
constexpr size_t kEncodedCap = size_t(4) << 20;

void encode_as(const char* ext, opencv_mat frame, const int* opts, size_t nopts, uint8_t primaries,
               uint8_t transfer) {
    // The encoders write into a caller-owned destination mat (newOpenCVEncoder).
    uint8_t* dst_buf = static_cast<uint8_t*>(malloc(kEncodedCap));
    opencv_mat dst = opencv_mat_create_empty_from_data(static_cast<int>(kEncodedCap), dst_buf);
    if (dst) {
        opencv_encoder enc = opencv_encoder_create(ext, dst);
        if (enc) {
            if (opencv_encoder_write(enc, frame, opts, nopts) &&
                opencv_mat_get_data(dst) == dst_buf) {
                const int len = opencv_mat_get_height(dst);
                if (len > 0 && static_cast<size_t>(len) <= kEncodedCap) {
                    (void)opencv_png_insert_cicp(dst_buf, len, kEncodedCap, primaries, transfer, 1, 1);
                }
            }
            opencv_encoder_release(enc);
        }
        opencv_mat_release(dst);
    }
    free(dst_buf);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return -1;  // the Go layer indexes buf[0]

    mh::Source src(data, size);
    if (!src.mat) return -1;

    opencv_decoder dec = opencv_decoder_create(src.mat);
    if (!dec) return -1;

    mh::Framebuffer fb;
    uint8_t* icc = static_cast<uint8_t*>(malloc(kIccBufferBytes));
    uint8_t* thumb_buf = static_cast<uint8_t*>(calloc(kThumbBytes, 1));

    if (opencv_decoder_read_header(dec)) {
        const int width = opencv_decoder_get_width(dec);
        const int height = opencv_decoder_get_height(dec);
        const int pixel_type = opencv_decoder_get_pixel_type(dec);
        const int orientation = opencv_decoder_get_orientation(dec);

        // openCVDecoder.ICC / CICP dispatch on the codec description.
        const char* desc = opencv_decoder_get_description(dec);
        uint8_t primaries = 1, transfer = 13, matrix = 0, full_range = 0;
        if (desc && strcmp(desc, "JPEG") == 0) {
            (void)opencv_decoder_get_jpeg_icc(src.bytes, size, icc, kIccBufferBytes);
        } else if (desc && strcmp(desc, "PNG") == 0) {
            const int icc_len = opencv_decoder_get_png_icc(src.bytes, size, icc, kIccBufferBytes);
            if (icc_len > 0 && static_cast<size_t>(icc_len) <= kIccBufferBytes) {
                (void)is_hdr_transfer_function(icc, icc_len);
                (void)icc_header_is_sane(icc, icc_len);
            }
            if (opencv_decoder_get_png_cicp(src.bytes, size, &primaries, &transfer, &matrix,
                                            &full_range)) {
                size_t profile_size = 0;
                (void)cicp_get_icc_profile(primaries, &profile_size);
                (void)cicp_is_hdr_transfer(transfer);
            }
        }

        if (fb.resize(width, height, pixel_type) &&  // else ErrBufTooSmall
            opencv_decoder_read_data(dec, fb.mat)) {
            opencv_mat_orientation_transform(static_cast<CVImageOrientation>(orientation), fb.mat);

            const int fw = opencv_mat_get_width(fb.mat);
            const int fh = opencv_mat_get_height(fb.mat);
            if (fw > 0 && fh > 0) {
                const int tw = fw < kThumbEdge ? fw : kThumbEdge;
                const int th = fh < kThumbEdge ? fh : kThumbEdge;
                int type = pixel_type;
                if (opencv_type_depth(type) > 8) type = opencv_type_convert_depth(type, CV_8U);
                opencv_mat thumb = opencv_mat_create_from_data(tw, th, type, thumb_buf, kThumbBytes);
                if (thumb) {
                    opencv_mat_resize(fb.mat, thumb, tw, th, CV_INTER_AREA);

                    const int jpeg_opts[] = {CV_IMWRITE_JPEG_QUALITY, 85, CV_IMWRITE_JPEG_PROGRESSIVE, 1};
                    const int png_opts[] = {CV_IMWRITE_PNG_COMPRESSION, 1};
                    encode_as(".jpeg", thumb, jpeg_opts, 4, primaries, transfer);
                    encode_as(".png", thumb, png_opts, 2, primaries, transfer);

                    opencv_mat_release(thumb);
                }

                opencv_mat crop = opencv_mat_crop(fb.mat, fw / 4, fh / 4, fw / 2 > 0 ? fw / 2 : 1,
                                                  fh / 2 > 0 ? fh / 2 : 1);
                if (crop) opencv_mat_release(crop);
            }
        }
    }

    opencv_decoder_release(dec);
    free(thumb_buf);
    free(icc);
    return 0;
}
