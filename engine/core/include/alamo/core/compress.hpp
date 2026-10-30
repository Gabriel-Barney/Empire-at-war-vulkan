// In-house lossless compressor for engine save files (N1 save size; no third-party code): LZ77 over a long window -
// a save's repeats (Lua VM images of the same scripts, chunks with the same layout) are typically far apart - with
// canonical Huffman coding of the tokens. Deflate-like, but with a 16 MiB window, matches up to 1 MiB and two
// repeat-distance codes (structured data: a long match, a few differing bytes, the same match continued).
//
// STREAM (bits packed LSB-first): blocks; each is 1 bit "last block", the literal/length code lengths, the distance
// code lengths, then Huffman-coded tokens up to the end-of-block symbol. After the last block, zero bits pad to a byte
// boundary and nothing follows.
//   code lengths (0 = unused, 1..15): per symbol, bit 0 = the previous symbol's length again (0 before the first),
//     bit 1 then the length in 4 bits. Codes are canonical (deflate's assignment: by length, then symbol).
//   literal / length alphabet (299): 0-255 a literal byte; 256 end of block; 257 + c a match whose length - 3 is
//     coded by bucket c, followed by a distance symbol.
//   distance alphabet (52): 0 the last match distance, 1 the one before (the two swap); 2 + c a new distance whose
//     value - 1 is coded by bucket c (it becomes the last, the old last the one before). Both start at 1.
//   bucket code of a value v: v < 8: code v, no extra bits; otherwise, with n = floor(log2 v): code 8 + 2 (n - 3) +
//     bit n - 1 of v, followed by the low n - 1 bits of v (extra bits, LSB-first).
//
// Determinism: the output is a pure function of the input bytes (integer arithmetic, fixed tie-breaks, positions
// never addresses, single-threaded), so MSVC / clang builds and every thread count produce identical bytes.
// Robustness: lz_decompress validates everything (code tables, symbols, distances, sizes, the end padding) and never
// reads or writes out of bounds, whatever the input.
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace alamo {

// Inputs up to 4 GiB - 1 (std::length_error beyond).
std::vector<uint8_t> lz_compress(std::span<const uint8_t> in);

// Decompresses a whole stream that must expand to exactly `size` bytes (`out` is replaced). False on malformed or
// trailing data; `out` is then unspecified.
bool lz_decompress(std::span<const uint8_t> in, size_t size, std::vector<uint8_t>& out);

}  // namespace alamo
