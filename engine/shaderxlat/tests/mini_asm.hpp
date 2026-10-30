// Tiny D3D9 asm -> bytecode assembler for tests: accepts the syntax disassemble() prints, so
// assemble(disassemble(p)) reproduces p's instruction tokens (used as a parser round-trip check on
// the real corpus) and synthetic shaders can be written as text.
//
//   "vs_2_0\n def c0, 1, 0, 0, 0\n dcl_position v0\n mova a0.x, v0.x\n mov r0, c[a0.x + 4]\n ..."
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::shaderxlat::test {

// Returns false and *err on a syntax error. Comments start with "//". END is appended.
bool assemble(std::string_view text, std::vector<uint32_t>& tokens, std::string* err);
std::vector<uint8_t> to_bytes(const std::vector<uint32_t>& tokens);

}  // namespace alamo::shaderxlat::test
