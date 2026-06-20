// Tensor dump helper for parity testing.
//
// Compile-time gated: define DUMP_TENSORS at build (`-DDUMP_TENSORS=ON` in
// CMake). Without it, every dump call is a no-op and the binary is
// indistinguishable from a production build.
//
// Runtime: set DUMP_TENSORS_DIR=/path/to/cpp/out, then the C++ binary in
// --parity-fixtures mode iterates fixtures/*.{wav,jpg}, runs each through
// the relevant pipeline, and writes the intermediate tensors as raw
// little-endian float32 to <DUMP_TENSORS_DIR>/<fixture_stem>__<stage>.bin.
// The diff_tensors.py script then compares each .bin to the matching .npy
// reference dumped by dump_python_reference.py.
//
// File naming convention (must match dump_python_reference.py):
//   <fixture_stem>__wuw_pcen.bin       (50*32 float32)
//   <fixture_stem>__wuw_dscnn.bin      (3 float32)
//   <fixture_stem>__pain_logmel5.bin   (50*32*5 float32)
//   <fixture_stem>__pain_yamnet.bin    (2 float32)
//   <fixture_stem>__fall_approx.bin    (5*4 float32  — one row per of 4 indices)
//   <fixture_stem>__fall_feat80.bin    (80 float32)
//   <fixture_stem>__fall_keypoints.bin (13*3 float32)
#pragma once

#include <cstdlib>
#include <cstdio>
#include <string>

// Set before running each fixture's pipeline (in --parity-fixtures mode);
// dumpTensorF32 prefixes every dump's filename with this. Globally
// accessible string — defined as `inline` so it lives in one TU only.
inline std::string g_parity_fixture_stem;

inline void dumpTensorF32(const char* stage,
                          const float* data, size_t count) {
#ifdef DUMP_TENSORS
    const char* dir = std::getenv("DUMP_TENSORS_DIR");
    if (!dir || !data || count == 0 || g_parity_fixture_stem.empty()) return;
    std::string path = std::string(dir) + "/" +
                       g_parity_fixture_stem + "__" + stage + ".bin";
    if (FILE* fp = std::fopen(path.c_str(), "wb")) {
        std::fwrite(data, sizeof(float), count, fp);
        std::fclose(fp);
    }
#else
    (void)stage; (void)data; (void)count;
#endif
}

template <typename Container>
inline void dumpTensorF32(const char* stage, const Container& c) {
    dumpTensorF32(stage, c.data(), c.size());
}
