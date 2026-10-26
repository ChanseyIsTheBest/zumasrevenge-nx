// h_821C5098.h: hand-written native of sub_821C5098, an uninitialized copy of 36-byte elements (MSVC's
// _Uninitialized_copy of a POD: r3 = first, r4 = last, r5 = dest; for each element, unless the destination pointer is
// 0, memcpy(dest, first, 36) through the C runtime's memcpy sub_825EA270; returns r3 = dest + 36 * count). Reference:
// app/generated/hot/n_821C5098.h (docs/hand-natives.md). The game's main thread's hottest generated function in the
// Ryujinx profile (out/renut_tests/emu/prof1: 105 samples, 5.9 % of that thread, its memcpy calls apart).
//
// Optimisation: one host memcpy of the whole span instead of a guest call a 36-byte element (each through memcpy's
// hook and native, sub_825EA270 / n_825F0AE0).
// Audit:
//   - taken only while memcpy's native is on (zumasrevenge_cpu_native_memcpy: the copy's calls then run n_825F0AE0,
//     which writes no register and copies each element as memmove), the span is a whole number of elements, dest is
//     not 0 and no destination pointer wraps to 0 (no element is skipped), neither range wraps past 4 GB nor straddles
//     0xE0000000 (host memory 4 KB higher from there on the PC), and the two ranges do not overlap: then the element
//     copies are independent and together are one copy of the span, in any order;
//   - - the context as the copy leaves it: r3 = dest + 36 * count, r4 = first + 36 * (count - 1) and r5 = 36 (the last
//     call's arguments, which memcpy's native leaves), r12 as the copy's dead definition (the entry lr), lr =
//     0x821C50D0 (set before each call), r1 unchanged, and the copy's back-chain store at r1 - 112 (REX_CHAIN_STORE);
//   - everything else (first == last, memcpy's native off, any of the cases above) runs the copy, n_821C5098::Native.
// Calls: none (the copy's calls are memcpy's native, reproduced), but the copy it falls back to calls, so the hook
// stays unguarded.
#pragma once

// n_821C5098.h (the copy) is included just before this file by app/generated/hot/localized_hooks.inc.

namespace zumasrevenge::hot::h_821C5098 {

template <bool kLog>
inline void Body(PPCContext& ctx, uint8_t* base) {
  const uint32_t first = ctx.r3.u32, last = ctx.r4.u32, dest = ctx.r5.u32;
  const uint32_t span = last - first;
  auto straddles = [](uint32_t a, uint32_t len) { return (a >= 0xE0000000u) != (a + len - 1 >= 0xE0000000u); };
  if (!g_enh_825EA270.active || first == last || span % 36 != 0 || dest == 0 ||
      uint64_t(dest) + span > 0x100000000ull || uint64_t(first) + span > 0x100000000ull || straddles(first, span) ||
      straddles(dest, span) || (first < dest + span && dest < first + span)) [[unlikely]] {
    n_821C5098::Native(ctx, base);
    return;
  }
  REX_FUNC_PROLOGUE();
  const uint32_t count = span / 36;
  PPCRegister l_r12 = ctx.r12;
  REX_DEAD_DEF(r12, l_r12.u64 = ctx.lr);
  const uint32_t ea = ctx.r1.u32 - 112;
  REX_CHAIN_STORE(REX_STORE_U32(ea, ctx.r1.u32));
  if constexpr (kLog) ::zumasrevenge::hot::LogBefore(base, dest, span);
  std::memcpy(Raw(base, dest), Raw(base, first), span);
  ctx.r4.u64 = ctx.r3.u64 + uint64_t(36) * (count - 1);
  ctx.r3.u64 = ctx.r5.u64 + uint64_t(36) * count;
  ctx.r5.s64 = 36;
  ctx.r12 = l_r12;
  ctx.lr = 0x821C50D0;
}

inline void Native(PPCContext& ctx, uint8_t* base) { Body<false>(ctx, base); }
inline void NativeLogged(PPCContext& ctx, uint8_t* base) { Body<true>(ctx, base); }

}  // namespace zumasrevenge::hot::h_821C5098
