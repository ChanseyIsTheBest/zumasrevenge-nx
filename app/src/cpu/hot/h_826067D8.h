// h_826067D8.h: hand-written native of sub_826067D8, the XDK's XMemSet (r3 = dst, r4 = the byte value in its low 8
// bits, r5 = n; returns r3 = dst): bytes one at a time below 16; else stvlx up to the 16-byte boundary, 16-byte stvx to
// the 128-byte one, dcbz128 lines (for a zero value; for another, dcbz128 four lines ahead and eight stvx a line),
// 16-byte stvx, then stvrx for the last bytes. Reference: app/generated/hot/n_826067D8.h (docs/hand-natives.md). The
// hottest generated function of the Ryujinx profile (out/renut_tests/emu/prof1: 361 samples, a game thread's 15 %).
//
// Optimisation: one host memset of [dst, dst + n), instead of the byte, 16-byte and 128-byte stores; the loops'
// register arithmetic in closed form.
// Audit:
//   - memory: every path writes each byte of [dst, dst + n) and nothing else, with the byte r4 & 0xFF: the n < 16
//     loop stores the low byte of extsb(r4); for n >= 16, v0 is that byte splatted (lvsl of r4 and of r4 >> 4, vslb by
//     4, vor, vspltb 0: the copy's own three operations are kept here, so v0 is the copy's); the head is the 16 - (dst
//     & 15) bytes to the boundary when dst is not aligned, then min(n' >> 4, the 16-byte units to the 128 boundary)
//     units (n' = n less the head), then (n'' >> 7) lines (n'' = n' less those units), then the 16-byte units of the
//     rest and its last (rest & 15) bytes. The non-zero path's dcbz128 at +512 only clears a line a later pass of the
//     same loop fills (j + 4 < lines), so the bytes at the end are the value's. The 16-byte stores advance by the word
//     at 0x82981FB4 (16 in the image): any other value takes the copy;
//   - registers: the copy writes back r3-r12, v0, v12 and v13 and leaves lr = 0x826067E0 (the restgprlr tail is its
//     own: r29-r31 are locals of the copy). Each is set here to the copy's value on the path taken (n == 0, n < 16, the
//     zero value's path and the other one), the counts as the copy's 64-bit adds over u32 trip counts;
//   - a range that wraps past 4 GB, or one on both sides of 0xE0000000 (host memory 4 KB higher from there on the PC),
//     takes the copy (n_826067D8::Native / NativeLogged).
#pragma once

// n_826067D8.h (the copy) is included just before this file by app/generated/hot/localized_hooks.inc.

namespace zumasrevenge::hot::h_826067D8 {

constexpr uint32_t kStrideVa = 0x82981FB4;  // the 16-byte stores' step (lis r11,0x8298; lwz r6,8116(r11))

template <bool kLog>
inline void Body(PPCContext& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, n = ctx.r5.u32;
  const bool wraps = uint64_t(dst) + n > 0x100000000ull;
  const bool splits = n && ((dst >= 0xE0000000u) != (dst + n - 1 >= 0xE0000000u));
  if (wraps || splits || (n >= 16 && Ld32(base, kStrideVa) != 16)) [[unlikely]] {
    if constexpr (kLog) n_826067D8::NativeLogged(ctx, base); else n_826067D8::Native(ctx, base);
    return;
  }
  REX_FUNC_PROLOGUE();
  const uint64_t lr = ctx.lr;
  ctx.lr = 0x826067E0;
  if (n < 16) {
    ctx.r12.u64 = lr;
    if (n == 0) {
      ctx.r10.u64 = ctx.r3.u64;
      ctx.r11.s64 = 0;
      return;
    }
    if constexpr (kLog) ::zumasrevenge::hot::LogBefore(base, dst, n);
    std::memset(Raw(base, dst), ctx.r4.u8, n);
    ctx.r10.s64 = ctx.r4.s8;
    ctx.r11.s64 = n;
    return;
  }
  // v13 / v12 / v0: the copy's own operations.
  PPCVRegister v0, v12, v13;
  simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_set1_epi8(char(0x4)));
  simde_mm_store_si128((simde__m128i*)v13.u8,
                       simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[(ctx.r4.u32 & 0xF) * 16]));
  PPCRegister r8, r9, r10, r11, r5, r4, r6, r7;
  r8.s64 = ctx.r4.s32 >> 4;
  simde_mm_store_si128((simde__m128i*)v12.u8,
                       simde_mm_load_si128((simde__m128i*)&VectorShiftTableL[(r8.u32 & 0xF) * 16]));
  {
    simde__m128i a = simde_mm_load_si128((simde__m128i*)v12.u8);
    simde__m128i b = simde_mm_load_si128((simde__m128i*)v0.u8);
    simde__m128i shift = simde_mm_and_si128(b, simde_mm_set1_epi8(0x7));
    simde_mm_store_si128((simde__m128i*)v0.u8, rex::ppc::simde_mm_sllv_epi8(a, shift));
  }
  simde_mm_store_si128((simde__m128i*)v0.u8, simde_mm_or_si128(simde_mm_load_si128((simde__m128i*)v13.u8),
                                                                simde_mm_load_si128((simde__m128i*)v0.u8)));
  simde_mm_store_si128((simde__m128i*)v0.u8,
                       simde_mm_shuffle_epi8(simde_mm_load_si128((simde__m128i*)v0.u8), simde_mm_set1_epi8(char(0xF))));
  // The head.
  r4 = ctx.r4;
  r5 = ctx.r5;
  r10 = ctx.r3;
  r11.s64 = static_cast<int64_t>(-ctx.r3.u64);
  r9.s64 = r11.s32 >> 4;
  r9.u64 = r9.u32 & 0x7;
  r11.u64 = r11.u32 & 0xF;
  if (r11.u32 != 0) {
    r5.u64 = r5.u64 - r11.u64;
    r10.u64 = r11.u64 + ctx.r3.u64;
  }
  // 16-byte units to the 128-byte boundary.
  r11.u64 = r5.u32 >> 4;
  if (r11.u32 < r9.u32) r9.u64 = r11.u64;
  r6.u64 = 16;
  r10.u64 = r10.u64 + r6.u64 * r9.u32;
  // 128-byte lines.
  r11.u64 = uint32_t(r9.u32 << 4);
  r9.u64 = r4.u32 & 0xFF;
  r7.u64 = r5.u64 - r11.u64;
  r11.u64 = r7.u32 >> 7;
  if (r9.s32 == 0) {
    r10.s64 = r10.s64 + int64_t(128) * r11.u32;
  } else {
    const uint32_t ahead = r11.u32 < 4 ? r11.u32 : 4;
    r8.u64 = ahead ? uint32_t((ahead - 1) << 7) : ahead;
    r9.s64 = 0;
    if (r11.u32 != 0) {
      r8.s64 = 112;
      r5.s64 = 32;
      r4.s64 = 48;
      r9.s64 = r11.u32;
      r10.s64 = r10.s64 + int64_t(128) * r11.u32;
    }
  }
  // The 16-byte units of the rest; its last bytes change no register.
  r11.u64 = uint32_t(r11.u32 << 7);
  r11.u64 = r7.u64 - r11.u64;
  if (r11.u32 >= 16) {
    r9.u64 = r11.u32 >> 4;
    r10.u64 = r10.u64 + r6.u64 * r9.u32;
    r11.s64 = r11.s64 - int64_t(16) * r9.u32;
  }
  if constexpr (kLog) ::zumasrevenge::hot::LogBefore(base, dst, n);
  std::memset(Raw(base, dst), ctx.r4.u8, n);  // before ctx.r4 takes the copy's last value (48 on the non-zero path)
  ctx.r4 = r4;
  ctx.r5 = r5;
  ctx.r6 = r6;
  ctx.r7 = r7;
  ctx.r8 = r8;
  ctx.r9 = r9;
  ctx.r10 = r10;
  ctx.r11 = r11;
  ctx.r12.u64 = lr;
  ctx.v0 = v0;
  ctx.v12 = v12;
  ctx.v13 = v13;
}

inline void Native(PPCContext& ctx, uint8_t* base) { Body<false>(ctx, base); }
inline void NativeLogged(PPCContext& ctx, uint8_t* base) { Body<true>(ctx, base); }

}  // namespace zumasrevenge::hot::h_826067D8
