# Converts a SPIR-V binary into a C++ header with the module as uint32 words.
# Usage: cmake -DIN=<file.spv> -DOUT=<file.h> -DSYM=<symbol> -P spv_to_header.cmake
file(READ "${IN}" hex HEX)
string(LENGTH "${hex}" n)
math(EXPR rem "${n} % 8")
if(n EQUAL 0 OR NOT rem EQUAL 0)
  message(FATAL_ERROR "${IN}: SPIR-V size is not a multiple of 4 bytes")
endif()
# little-endian bytes -> words
string(REGEX REPLACE "([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])([0-9a-f][0-9a-f])"
       "0x\\4\\3\\2\\1u," words "${hex}")
string(REGEX REPLACE "((0x[0-9a-f]+u,)(0x[0-9a-f]+u,)(0x[0-9a-f]+u,)(0x[0-9a-f]+u,)(0x[0-9a-f]+u,)(0x[0-9a-f]+u,)(0x[0-9a-f]+u,)(0x[0-9a-f]+u,))"
       "\\1\n" words "${words}")
file(WRITE "${OUT}.tmp"
  "// Generated from ${SYM}.spv by spv_to_header.cmake. Do not edit.\n"
  "#pragma once\n#include <cstdint>\n"
  "inline constexpr uint32_t ${SYM}[] = {\n${words}\n};\n")
file(RENAME "${OUT}.tmp" "${OUT}")
