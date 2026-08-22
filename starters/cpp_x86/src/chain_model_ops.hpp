// A portable model of the x86 SHA-NI / SSE ops used by chain_x86_core.hpp.
//
// Each function is transcribed from the Intel SDM operation pseudocode and is
// deliberately written in terms of the SDM's own bit-range names, NOT in terms
// of what the caller wants. That independence is the whole point: if the model
// were written to make the caller work, it would validate nothing.
//
// Lane convention throughout: w[0] is bits 31:0 (the LOW dword), w[3] is bits
// 127:96 (the HIGH dword). Byte b of the register is w[b/4] >> (8*(b%4)).
#pragma once

#include <cstdint>
#include <cstring>

namespace obsidio {
namespace chain {

struct ModelOps {
  struct T {
    std::uint32_t w[4];
  };

  // -- helpers -------------------------------------------------------------
  static std::uint8_t byte_at(const T& x, int i) {
    return static_cast<std::uint8_t>(x.w[i / 4] >> (8 * (i % 4)));
  }
  static void set_byte(T& x, int i, std::uint8_t v) {
    const int sh = 8 * (i % 4);
    x.w[i / 4] = (x.w[i / 4] & ~(0xffu << sh)) | (std::uint32_t(v) << sh);
  }
  static std::uint32_t ror(std::uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
  }
  // SHA-256 sigma functions, as the SDM defines them for these instructions.
  static std::uint32_t s0(std::uint32_t x) {
    return ror(x, 7) ^ ror(x, 18) ^ (x >> 3);
  }
  static std::uint32_t s1(std::uint32_t x) {
    return ror(x, 17) ^ ror(x, 19) ^ (x >> 10);
  }
  static std::uint32_t S0(std::uint32_t x) {
    return ror(x, 2) ^ ror(x, 13) ^ ror(x, 22);
  }
  static std::uint32_t S1(std::uint32_t x) {
    return ror(x, 6) ^ ror(x, 11) ^ ror(x, 25);
  }
  static std::uint32_t CH(std::uint32_t e, std::uint32_t f, std::uint32_t g) {
    return (e & f) ^ (~e & g);
  }
  static std::uint32_t MAJ(std::uint32_t a, std::uint32_t b, std::uint32_t c) {
    return (a & b) ^ (a & c) ^ (b & c);
  }

  // -- SHA256RNDS2 dest, src, XMM0 -----------------------------------------
  // SDM: A_0 := SRC2[127:96]  B_0 := SRC2[95:64]  C_0 := SRC1[127:96]
  //      D_0 := SRC1[95:64]   E_0 := SRC2[63:32]  F_0 := SRC2[31:0]
  //      G_0 := SRC1[63:32]   H_0 := SRC1[31:0]
  //      WK0 := XMM0[31:0]    WK1 := XMM0[63:32]
  //      two rounds, then DEST := {A_2, B_2, E_2, F_2}
  // Here `dest` is SRC1 and `src` is SRC2.
  static T rnds2(T dest, T src, T wk) {
    std::uint32_t A = src.w[3], B = src.w[2], E = src.w[1], F = src.w[0];
    std::uint32_t C = dest.w[3], D = dest.w[2], G = dest.w[1], H = dest.w[0];
    const std::uint32_t WK[2] = {wk.w[0], wk.w[1]};

    for (int i = 0; i < 2; ++i) {
      const std::uint32_t common = CH(E, F, G) + S1(E) + WK[i] + H;
      const std::uint32_t A_next = common + MAJ(A, B, C) + S0(A);
      const std::uint32_t E_next = common + D;
      H = G; G = F; F = E; E = E_next;
      D = C; C = B; B = A; A = A_next;
    }
    T r;
    r.w[3] = A; r.w[2] = B; r.w[1] = E; r.w[0] = F;
    return r;
  }

  // -- SHA256MSG1 dest, src -------------------------------------------------
  // SDM: W4 := SRC[31:0]; W3 := DEST[127:96]; W2 := DEST[95:64];
  //      W1 := DEST[63:32]; W0 := DEST[31:0]
  //      DEST[127:96] := W3 + sigma0(W4)   DEST[95:64] := W2 + sigma0(W3)
  //      DEST[63:32]  := W1 + sigma0(W2)   DEST[31:0]  := W0 + sigma0(W1)
  static T msg1(T dest, T src) {
    const std::uint32_t W4 = src.w[0];
    const std::uint32_t W3 = dest.w[3], W2 = dest.w[2];
    const std::uint32_t W1 = dest.w[1], W0 = dest.w[0];
    T r;
    r.w[3] = W3 + s0(W4);
    r.w[2] = W2 + s0(W3);
    r.w[1] = W1 + s0(W2);
    r.w[0] = W0 + s0(W1);
    return r;
  }

  // -- SHA256MSG2 dest, src -------------------------------------------------
  // SDM: W14 := SRC[95:64]; W15 := SRC[127:96]
  //      W16 := DEST[31:0]   + sigma1(W14)
  //      W17 := DEST[63:32]  + sigma1(W15)
  //      W18 := DEST[95:64]  + sigma1(W16)
  //      W19 := DEST[127:96] + sigma1(W17)
  //      DEST := {W19, W18, W17, W16}
  static T msg2(T dest, T src) {
    const std::uint32_t W14 = src.w[2], W15 = src.w[3];
    const std::uint32_t W16 = dest.w[0] + s1(W14);
    const std::uint32_t W17 = dest.w[1] + s1(W15);
    const std::uint32_t W18 = dest.w[2] + s1(W16);
    const std::uint32_t W19 = dest.w[3] + s1(W17);
    T r;
    r.w[0] = W16; r.w[1] = W17; r.w[2] = W18; r.w[3] = W19;
    return r;
  }

  // -- plain SSE data movement ---------------------------------------------
  static T add(T a, T b) {
    T r;
    for (int i = 0; i < 4; ++i) r.w[i] = a.w[i] + b.w[i];
    return r;
  }
  // _mm_shuffle_epi32(a, imm): dst[i] := a[(imm >> 2i) & 3]
  static T shuffle32(T a, int imm) {
    T r;
    for (int i = 0; i < 4; ++i) r.w[i] = a.w[(imm >> (2 * i)) & 3];
    return r;
  }
  static T shuf_0e(T a) { return shuffle32(a, 0x0E); }
  static T shuf_1b(T a) { return shuffle32(a, 0x1B); }

  // _mm_alignr_epi8(hi, lo, 4): concat hi:lo, shift right 4 bytes, keep low 16.
  static T alignr4(T hi, T lo) {
    T r{};
    for (int i = 0; i < 16; ++i) {
      const int src = i + 4;  // byte index into the 32-byte concatenation
      set_byte(r, i, src < 16 ? byte_at(lo, src) : byte_at(hi, src - 16));
    }
    return r;
  }
  static T unpacklo64(T a, T b) {
    T r; r.w[0] = a.w[0]; r.w[1] = a.w[1]; r.w[2] = b.w[0]; r.w[3] = b.w[1];
    return r;
  }
  static T unpackhi64(T a, T b) {
    T r; r.w[0] = a.w[2]; r.w[1] = a.w[3]; r.w[2] = b.w[2]; r.w[3] = b.w[3];
    return r;
  }
  // _mm_unpacklo_epi8(a, b): a0,b0,a1,b1,... from the low 8 bytes of each.
  static T unpacklo8(T a, T b) {
    T r{};
    for (int i = 0; i < 8; ++i) {
      set_byte(r, 2 * i, byte_at(a, i));
      set_byte(r, 2 * i + 1, byte_at(b, i));
    }
    return r;
  }
  static T unpackhi8(T a, T b) {
    T r{};
    for (int i = 0; i < 8; ++i) {
      set_byte(r, 2 * i, byte_at(a, 8 + i));
      set_byte(r, 2 * i + 1, byte_at(b, 8 + i));
    }
    return r;
  }
  // _mm_srli_epi16(a, 4) -- note this is a 16-bit shift, so bits cross the
  // byte boundary; callers mask afterwards, exactly as on real hardware.
  static T srli16_4(T a) {
    T r;
    for (int i = 0; i < 4; ++i) {
      const std::uint32_t lo = (a.w[i] & 0xffffu) >> 4;
      const std::uint32_t hi = ((a.w[i] >> 16) & 0xffffu) >> 4;
      r.w[i] = lo | (hi << 16);
    }
    return r;
  }
  static T and_(T a, T b) {
    T r;
    for (int i = 0; i < 4; ++i) r.w[i] = a.w[i] & b.w[i];
    return r;
  }
  // _mm_shuffle_epi8(tbl, idx): dst[i] := idx[i] & 0x80 ? 0 : tbl[idx[i] & 15]
  static T shuffle_bytes(T tbl, T idx) {
    T r{};
    for (int i = 0; i < 16; ++i) {
      const std::uint8_t s = byte_at(idx, i);
      set_byte(r, i, (s & 0x80) ? 0 : byte_at(tbl, s & 0x0f));
    }
    return r;
  }
  static T set8(std::uint8_t v) {
    T r;
    const std::uint32_t x = 0x01010101u * v;
    for (int i = 0; i < 4; ++i) r.w[i] = x;
    return r;
  }
  static T bswap32(T a) {
    T r;
    for (int i = 0; i < 4; ++i) {
      r.w[i] = ((a.w[i] & 0x000000ffu) << 24) | ((a.w[i] & 0x0000ff00u) << 8) |
               ((a.w[i] & 0x00ff0000u) >> 8) | ((a.w[i] & 0xff000000u) >> 24);
    }
    return r;
  }
  static T hex_table() {
    T r{};
    const char* h = "0123456789abcdef";
    for (int i = 0; i < 16; ++i) set_byte(r, i, std::uint8_t(h[i]));
    return r;
  }
  static T loadu(const std::uint32_t* p) {
    T r;
    for (int i = 0; i < 4; ++i) r.w[i] = p[i];
    return r;
  }
  static T loadu_bytes(const char* p) {
    T r;
    std::memcpy(r.w, p, 16);
    return r;
  }
  static void storeu_bytes(char* p, T a) { std::memcpy(p, a.w, 16); }
};

}  // namespace chain
}  // namespace obsidio
