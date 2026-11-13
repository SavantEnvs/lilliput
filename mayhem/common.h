// Shared helpers for the lilliput fuzz harnesses. They mirror how lilliput's Go layer drives the cgo
// wrappers: input bytes wrapped in a 1-row CV_8U mat, decoded frames written into a fixed-size,
// caller-owned framebuffer (Framebuffer.resizeMat in opencv.go).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "opencv.hpp"

namespace mh {

// Capacity of the caller-owned framebuffer: 2048x2048 RGBA, the "maximum image" an embedder
// configures through NewFramebuffer. opencv_mat_create_from_data() returns NULL when a header does
// not fit (the Go layer's ErrBufTooSmall), which is how oversized headers are refused.
constexpr size_t kFramebufferBytes = size_t(2048) * 2048 * 4;

// Output buffer handed to the encoders (the Go layer's dst []byte).
constexpr size_t kEncodeBufferBytes = size_t(16) << 20;

struct Source {
    uint8_t* bytes = nullptr;
    opencv_mat mat = nullptr;

    // Exact-size heap copy so ASan sees any read past the end of the input.
    explicit Source(const uint8_t* data, size_t size) {
        bytes = static_cast<uint8_t*>(malloc(size));
        memcpy(bytes, data, size);
        mat = opencv_mat_create_from_data(static_cast<int>(size), 1, CV_8U, bytes, size);
    }
    ~Source() {
        if (mat) opencv_mat_release(mat);
        free(bytes);
    }
    Source(const Source&) = delete;
    Source& operator=(const Source&) = delete;
};

struct Framebuffer {
    uint8_t* buf;
    opencv_mat mat = nullptr;

    // Fresh zeroed storage per iteration, so no pixel data leaks from one input to the next.
    Framebuffer() : buf(static_cast<uint8_t*>(calloc(kFramebufferBytes, 1))) {}
    ~Framebuffer() {
        if (mat) opencv_mat_release(mat);
        free(buf);
    }
    Framebuffer(const Framebuffer&) = delete;
    Framebuffer& operator=(const Framebuffer&) = delete;

    // Same steps as Framebuffer.resizeMat: depths above 8 bits are converted to CV_8U.
    bool resize(int width, int height, int pixel_type) {
        if (mat) {
            opencv_mat_release(mat);
            mat = nullptr;
        }
        if (opencv_type_depth(pixel_type) > 8) {
            pixel_type = opencv_type_convert_depth(pixel_type, CV_8U);
        }
        mat = opencv_mat_create_from_data(width, height, pixel_type, buf, kFramebufferBytes);
        return mat != nullptr;
    }
};

}  // namespace mh
