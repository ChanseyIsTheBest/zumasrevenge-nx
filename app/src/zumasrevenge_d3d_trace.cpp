// D3D call trace (PC test aid; zumasrevenge-nx 2026-10-27: Renut's shader dump for Zuma's Revenge, docs/
// zumasrevenge-notes.md): Direct3D_CreateDevice and the shader / declaration creators are hooked. With
// zumasrevenge_d3d_trace = <game frame>, the log gets, per function: its calls up to that frame, whether r3 was the device
// (the word Direct3D_CreateDevice fills through its r8), its first arguments and caller; and the order of the calls
// during that frame; every shader container created goes to out/zr_shaders. Off by default; the PC build without Renut
// only. (feedingfrenzy2-nx's trace, through zuma-nx.)

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "zumasrevenge_capture.h"

REXCVAR_DEFINE_INT32(zumasrevenge_d3d_trace, 0, "ZumasRevenge",
                     "PC test aid: log the Direct3D API calls up to and during this game frame (0 = off)");

namespace {

constexpr uint32_t kCreateDevice = 0x82321C30u;  // zumasrevenge-nx: Direct3D_CreateDevice
uint32_t g_device_slot = 0;  // Direct3D_CreateDevice's r8
struct Stat {
  uint32_t addr = 0;
  uint64_t calls = 0, device_calls = 0;
  uint32_t first[3][6] = {};
};
std::mutex g_mutex;
std::vector<Stat> g_stats;
std::vector<std::string> g_frame_calls;
bool g_dumped = false;

uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t be;
  std::memcpy(&be, base + a, 4);
  return __builtin_bswap32(be);
}

void Trace(uint32_t addr, PPCContext& ctx, uint8_t* base) {
  const int32_t at = REXCVAR_GET(zumasrevenge_d3d_trace);
  if (at <= 0 || g_dumped) return;
  const uint64_t frame = g_zumasrevenge_game_frames.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_mutex);
  if (addr == kCreateDevice) g_device_slot = ctx.r8.u32;
  const uint32_t device = g_device_slot ? Load32(base, g_device_slot) : 0;
  Stat* s = nullptr;
  for (auto& x : g_stats) if (x.addr == addr) s = &x;
  if (!s) {
    g_stats.push_back({});
    s = &g_stats.back();
    s->addr = addr;
  }
  const bool is_dev = device && ctx.r3.u32 == device;
  if (s->calls < 3) {
    uint32_t* f = s->first[s->calls];
    f[0] = ctx.r3.u32; f[1] = ctx.r4.u32; f[2] = ctx.r5.u32; f[3] = ctx.r6.u32; f[4] = ctx.r7.u32;
    f[5] = uint32_t(ctx.lr);
  }
  // The shader containers (CreatePixelShader 82314360, CreateVertexShader 82314548) and the vertex elements
  // (CreateVertexDeclaration 82312D68): their first 96 bytes, on the first three calls.
  if (s->calls < 3 && (addr == 0x82314360u || addr == 0x82314548u || addr == 0x82312D68u) && ctx.r3.u32) {
    std::string hex;
    for (uint32_t o = 0; o < 96; o += 4) hex += fmt::format(" {:08X}", Load32(base, ctx.r3.u32 + o));
    REXLOG_INFO("[d3dtrace] {:08X} r3 {:08X}:{}", addr, ctx.r3.u32, hex);
  }
  // Every shader container created (virtual part + physical part) to out/zr_shaders/<ps|vs>_<n>.bin, for Renut's
  // shader dump (tools/renut_shader_dump.py names them by XXH3).
  if ((addr == 0x82314360u || addr == 0x82314548u) && ctx.r3.u32) {
    const uint32_t c = ctx.r3.u32;
    const uint32_t size = Load32(base, c + 4) + Load32(base, c + 8);
    if (size > 0 && size < 0x40000) {
      static int n = 0;
      const std::string path = fmt::format("out/zr_shaders/{}_{:02}.bin", addr == 0x82314360u ? "ps" : "vs", n++);
      std::filesystem::create_directories("out/zr_shaders");
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(base + c, 1, size, f);
        std::fclose(f);
      }
    }
  }
  ++s->calls;
  if (is_dev) ++s->device_calls;
  if (frame == uint64_t(at) && g_frame_calls.size() < 4000) {
    g_frame_calls.push_back(fmt::format("{:08X}{} r4={:X} r5={:X} r6={:X} lr={:08X}", addr, is_dev ? " dev" : "",
                                        ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, uint32_t(ctx.lr)));
  }
  if (frame > uint64_t(at)) {
    g_dumped = true;
    REXLOG_INFO("[d3dtrace] device 0x{:08X}; {} functions called up to frame {}", device, g_stats.size(), at);
    for (const auto& x : g_stats) {
      std::string first;
      for (uint64_t k = 0; k < x.calls && k < 3; ++k)
        first += fmt::format(" [{:X} {:X} {:X} {:X} {:X} lr {:08X}]", x.first[k][0], x.first[k][1], x.first[k][2],
                             x.first[k][3], x.first[k][4], x.first[k][5]);
      REXLOG_INFO("[d3dtrace] {:08X} calls {} (r3 = device {}):{}", x.addr, x.calls, x.device_calls, first);
    }
    REXLOG_INFO("[d3dtrace] frame {}: {} calls", at, g_frame_calls.size());
    for (const auto& c : g_frame_calls) REXLOG_INFO("[d3dtrace]   {}", c);
  }
}

}  // namespace

#define FF_D3D_TRACE(ADDR)                            extern "C" REX_FUNC(__imp__sub_##ADDR);            REX_HOOK_RAW(sub_##ADDR) {                            Trace(0x##ADDR##u, ctx, base);                     __imp__sub_##ADDR(ctx, base);                    }

// zumasrevenge-nx (2026-10-27): the device's creation and the shader / declaration creators (renut_hook_remap_zr.py's
// mapping), for the shader dump.
FF_D3D_TRACE(82321C30)  // Direct3D_CreateDevice
FF_D3D_TRACE(82314360)  // D3DDevice_CreatePixelShader
FF_D3D_TRACE(82314548)  // D3DDevice_CreateVertexShader
FF_D3D_TRACE(82312D68)  // D3DDevice_CreateVertexDeclaration
