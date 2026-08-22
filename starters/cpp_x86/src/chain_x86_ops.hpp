// Real x86 SHA-NI / SSSE3 intrinsics for chain_x86_core.hpp.
//
// Every function here is a one-line wrapper, on purpose: the algorithm lives in
// the core header, so this file and chain_model_ops.hpp are the only two places
// the instruction semantics are pinned down, and they can be read side by side.
//
// This header is only included from chain_x86.cpp, which CMake compiles with
// -msha -mssse3 -msse4.1. Nothing in it runs until __builtin_cpu_supports("sha")
// has said yes at run time.
#pragma once

#if defined(__x86_64__) || defined(__i386__)

#include <immintrin.h>

#include <cstdint>

namespace obsidio {
namespace chain {

struct X86Ops {
  using T = __m128i;

  static T add(T a, T b) { return _mm_add_epi32(a, b); }
  static T shuf_0e(T a) { return _mm_shuffle_epi32(a, 0x0E); }
  static T shuf_1b(T a) { return _mm_shuffle_epi32(a, 0x1B); }

  static T rnds2(T dest, T src, T wk) {
    return _mm_sha256rnds2_epu32(dest, src, wk);
  }
  static T msg1(T a, T b) { return _mm_sha256msg1_epu32(a, b); }
  static T msg2(T a, T b) { return _mm_sha256msg2_epu32(a, b); }

  static T alignr4(T hi, T lo) { return _mm_alignr_epi8(hi, lo, 4); }
  static T unpacklo64(T a, T b) { return _mm_unpacklo_epi64(a, b); }
  static T unpackhi64(T a, T b) { return _mm_unpackhi_epi64(a, b); }
  static T unpacklo8(T a, T b) { return _mm_unpacklo_epi8(a, b); }
  static T unpackhi8(T a, T b) { return _mm_unpackhi_epi8(a, b); }
  static T srli16_4(T a) { return _mm_srli_epi16(a, 4); }
  static T and_(T a, T b) { return _mm_and_si128(a, b); }
  static T shuffle_bytes(T tbl, T idx) { return _mm_shuffle_epi8(tbl, idx); }
  static T set8(std::uint8_t v) {
    return _mm_set1_epi8(static_cast<char>(v));
  }

  // Byte-reverse within each 32-bit lane: result byte i = src byte mask[i].
  static T bswap32(T a) {
    const T mask = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4,
                                 11, 10, 9, 8, 15, 14, 13, 12);
    return _mm_shuffle_epi8(a, mask);
  }

  static T hex_table() {
    return _mm_setr_epi8('0', '1', '2', '3', '4', '5', '6', '7',
                         '8', '9', 'a', 'b', 'c', 'd', 'e', 'f');
  }

  static T loadu(const std::uint32_t* p) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
  }
  static T loadu_bytes(const char* p) {
    return _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));
  }
  static void storeu_bytes(char* p, T a) {
    _mm_storeu_si128(reinterpret_cast<__m128i*>(p), a);
  }
};

}  // namespace chain
}  // namespace obsidio

#endif  // x86
