# JPEG EXIF heap-use-after-free (OpenCV `ExifReader` over libjpeg)

`jpeg-exif-uaf.jpg` is a mutated `data/sunrise.jpg` (2539 bytes). Replay:

    /mayhem/fuzz_opencv-standalone mayhem/fuzz_opencv/known-findings/jpeg-exif-uaf.jpg

ASan reports `heap-use-after-free`, READ of size 90, in `memcpy` called from
`cv::ExifReader::parseExif` <- `cv::JpegDecoder::readHeader` <- `opencv_decoder_read_header`
(`opencv.cpp`, the `read_header` wrapper). The block was freed by libjpeg `free_pool` (`jmemmgr.c`).

The bug is in the prebuilt OpenCV/libjpeg under `deps/linux/amd64`, reached through lilliput's
`opencv_decoder_read_header`: the EXIF APP1 payload is copied after the libjpeg pool that owns it has
been released when the image has a malformed APP1 segment. It is not in the harness (the input is an
ordinary byte buffer decoded once). It halts the fuzzer within a few hundred executions, so it is kept
here and not in `testsuite/`.
