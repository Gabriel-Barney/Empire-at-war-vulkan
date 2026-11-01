// Shared bits of alamo_shaderxlat_tests.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "alamo/core/test_support.hpp"
#include "alamo/shaderxlat/bytecode.hpp"
#include "alamo/shaderxlat/translate.hpp"

namespace alamo::shaderxlat::test {

// One shader program under test: synthetic (asm text) or from the installed game.
struct Blob {
    std::string name;           // synthetic name or 12-hex SHA-1 prefix (shaders_decompiled.zip naming)
    std::string origin;         // "synthetic" or "<game>/<EFFECT>"
    std::vector<uint8_t> bytes;
    TranslateOptions options;
};

// Synthetic programs covering every opcode / modifier family the translator supports, including
// those absent from the vanilla corpus (flow control, predication, texm3x3*, texbem, bem, ...).
std::vector<Blob> synthetic_blobs();

// Real FXO shader blobs (unique by content) from ALAMO_FOC_DATA and ALAMO_EAW_DATA. Empty when
// neither is set (or built without the VFS). *note receives a one-line description.
std::vector<Blob> corpus_blobs(std::string* note);

std::string sha1_hex(const std::vector<uint8_t>& data);
// The 61 blobs vkd3d-shader 1.19 fails on (docs/shaders/validation.json), by 12-hex SHA-1 prefix,
// with the reason.
struct HardCase {
    const char* sha12;
    const char* reason;
};
const std::vector<HardCase>& vkd3d_hard_cases();

// GPU differential test (gpu_diff.cpp). Returns the process exit code.
int run_gpu_tests(const std::vector<Blob>& synthetic, const std::vector<Blob>& corpus);

}  // namespace alamo::shaderxlat::test
