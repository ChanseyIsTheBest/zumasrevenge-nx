// Touch controls (the user's request, 2026-10-27: "I want all menu items clickable and tapping in levels shoots the
// ball in that direction"). The Switch's touch screen, and the mouse on the PC, become the game's own touches.
//
// Zuma's Revenge's framework is PopCap's SexyAppFramework of Bejeweled 3 (bejeweled3-nx's bejeweled3_touch.cpp; this
// file is its generic part): the Xbox 360 build still links the touch path (out/touch/notes.txt):
//   - SexyAppBase's vtable (0x820B04AC) slots 96 / 97 / 98 load mWidgetManager (+880) and jump to WidgetManager's
//     TouchBegan (sub_8241EFA0), TouchEnded (sub_8241F1E8) and TouchMoved (sub_8241F0D0);
//   - each takes a SexyTouch (+8 x, +12 y, +16 previous x, +20 previous y, in the game's coordinates). TouchBegan moves
//     the mouse there (WidgetManager::MouseMove sub_8241EE50), finds the widget under it (GetWidgetAt sub_8241E950),
//     makes the point local to it and calls the widget's TouchBegan; a widget that does not override it gets
//     MouseDown(x, y, 1) (Widget's defaults), the PC game's mouse handling: the buttons and dialogs, and the level's
//     Board (vtable 0x820032BC: its own MouseMove 8218F790 and MouseDown 821AA798, the PC version's aim and shot).
// Nothing calls those three on the console. This file calls them on the game's own thread, between frames (from the
// D3DDevice_Swap hook, zumasrevenge_frame_hooks.cpp), with the touches queued since the previous frame.
//
// The app object: SexyAppBase's constructor (sub_8243FB90: stores the SexyAppBase vtables 0x820B04AC / 0x820B049C at
// +0 / +4; called once, from SexyApp's sub_824207E8) is hooked to keep `this`. (sub_8243F260, which also names those
// vtables, is the destructor.)

#include "zumasrevenge_touch.h"

#include "zumasrevenge_input_script.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#if REX_PLATFORM_SWITCH
#include <switch.h>
#endif

REXCVAR_DEFINE_BOOL(zumasrevenge_touch, true, "ZumasRevenge",
                    "Touch controls: the Switch's touch screen (the mouse on the PC) taps the game's buttons and aims "
                    "and shoots in a level");
REXCVAR_DEFINE_INT32(zumasrevenge_touch_width, 1280, "ZumasRevenge",
                     "The game's coordinate width the touch screen (or window) is scaled to, when the game has not set "
                     "its mouse rectangle");
REXCVAR_DEFINE_INT32(zumasrevenge_touch_height, 720, "ZumasRevenge",
                     "The game's coordinate height the touch screen (or window) is scaled to, when the game has not set "
                     "its mouse rectangle");
REXCVAR_DEFINE_BOOL(zumasrevenge_touch_log, false, "ZumasRevenge", "Log every touch handed to the game");
REXCVAR_DEFINE_STRING(zumasrevenge_touch_script, "", "ZumasRevenge",
                      "Test runs: scripted touches, comma-separated \"<frame>:<down|move|up>:<x>:<y>\" in 1280x720 "
                      "screen pixels, frames counted by this file's dispatch (one a swap); empty = off");

namespace {

constexpr uint32_t kAppWidgetManager = 880;  // SexyAppBase::mWidgetManager (the slot-96..98 thunks load it)
constexpr uint32_t kWidgetMouseVisible = 0x51;  // Widget::mMouseVisible (mVisible +0x50, mDisabled +0x52)
// RTTI (out/touch/notes.txt): Sexy::ButtonCalloutWidget, the button prompts' layer, and Sexy::LoadingScreen, the title.
constexpr uint32_t kButtonCalloutVtable = 0x820090DC;
constexpr uint32_t kLoadingScreenVtable = 0x82020B3C;
constexpr uint32_t kBoardVtable = 0x820032BC;  // Sexy::Board, a level
constexpr uint32_t kBoardGun = 1136;         // Board's Gun (built by sub_821F2948 from Board's sub_8219DBE8)
constexpr uint32_t kGunVtable = 0x82012848;  // Sexy::Gun
constexpr uint32_t kGunCentre = 0x1DC;       // two floats, the board's coordinates
constexpr uint32_t kMainMenuVtable = 0x8202148C;  // Sexy::MainMenu
constexpr uint32_t kMainMenuSelection = 228;
constexpr uint32_t kWidgetMouseDown = 0x82416470;  // Widget::MouseDown(x, y, clicks), slot 54: no handling of its own
// The main menu's entries in its (the screen's) coordinates, from the game's 1280x720 picture (`touch4`).
struct MenuEntry {
  int32_t left, top, right, bottom;
  uint32_t bit;
};
constexpr MenuEntry kMainMenuEntries[] = {
    {215, 140, 420, 505, 1},     // Spirit Temple
    {500, 125, 845, 230, 2},     // Adventure
    {500, 230, 845, 330, 4},     // Weekly Challenge
    {500, 330, 845, 430, 8},     // Iron Frog
    {500, 430, 845, 530, 16},    // Boss Rush
    {970, 125, 1065, 222, 32},   // the first icon (the arrow)
    {970, 222, 1065, 320, 64},   // leaderboards
    {970, 320, 1065, 418, 128},  // trophies
    {970, 418, 1065, 516, 256},  // help
    {970, 516, 1065, 600, 512},  // X
};
constexpr uint16_t kPadA = 0x1000;  // XINPUT_GAMEPAD_A

std::atomic<uint32_t> g_app{0};

enum class Phase : uint8_t { kBegan, kMoved, kEnded };
struct Touch {
  Phase phase;
  float x, y;  // 0..1 of the screen / window
};

std::mutex g_mutex;
std::deque<Touch> g_queue;
bool g_down = false;  // a touch is in progress (as the game saw it)
int32_t g_last_x = 0, g_last_y = 0;

void Push(Phase phase, float x, float y) {
  if (!REXCVAR_GET(zumasrevenge_touch)) return;
  x = std::clamp(x, 0.0f, 1.0f);
  y = std::clamp(y, 0.0f, 1.0f);
  std::lock_guard<std::mutex> lock(g_mutex);
  // Coalesce moves: only the newest position of a run of moves matters.
  if (phase == Phase::kMoved && !g_queue.empty() && g_queue.back().phase == Phase::kMoved) {
    g_queue.back().x = x;
    g_queue.back().y = y;
    return;
  }
  if (g_queue.size() < 64) g_queue.push_back({phase, x, y});
}

inline void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  const uint32_t be = __builtin_bswap32(value);
  std::memcpy(base + address, &be, 4);
}
inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t be;
  std::memcpy(&be, base + address, 4);
  return __builtin_bswap32(be);
}
inline float LoadF32(const uint8_t* base, uint32_t address) {
  const uint32_t w = Load32(base, address);
  float f;
  std::memcpy(&f, &w, 4);
  return f;
}

// Whether the guest has [address, address + size) mapped, for a word that may not hold a pointer (bejeweled3-nx).
bool Readable(uint32_t address, uint32_t size) {
  auto* kernel = rex::system::kernel_state();
  auto* memory = kernel ? kernel->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  return heap && heap->QueryRangeAccess(address, address + size - 1) != rex::memory::PageAccess::kNoAccess;
}

// Test runs: zumasrevenge_touch_script, parsed once; the events due at each dispatch are queued as if touched.
struct ScriptedTouch {
  uint64_t frame;
  Phase phase;
  float x, y;
};

void PollScript(uint64_t frame) {
  static std::vector<ScriptedTouch> script;
  static size_t next = 0;
  static bool parsed = false;
  if (!parsed) {
    parsed = true;
    const std::string text = REXCVAR_GET(zumasrevenge_touch_script);
    size_t at = 0;
    while (at < text.size()) {
      size_t end = text.find(',', at);
      if (end == std::string::npos) end = text.size();
      const std::string item = text.substr(at, end - at);
      at = end + 1;
      unsigned long long when = 0;
      char kind[8] = {};
      float x = 0, y = 0;
      if (std::sscanf(item.c_str(), "%llu:%7[a-z]:%f:%f", &when, kind, &x, &y) == 4) {
        const std::string k = kind;
        const Phase phase = k == "down" ? Phase::kBegan : k == "move" ? Phase::kMoved : Phase::kEnded;
        script.push_back({when, phase, x / 1280.0f, y / 720.0f});
      } else {
        REXLOG_WARN("[touch] zumasrevenge_touch_script: \"{}\" is not <frame>:<down|move|up>:<x>:<y>", item);
      }
    }
    if (!script.empty()) REXLOG_INFO("[touch] scripted touches: {}", script.size());
  }
  while (next < script.size() && script[next].frame <= frame) {
    const ScriptedTouch& t = script[next++];
    Push(t.phase, t.x, t.y);
  }
}

#if REX_PLATFORM_SWITCH
// The Switch: the touch screen is polled once a frame (1280x720 panel coordinates in handheld and docked mode alike;
// docked, nothing touches it). A release reaches the game where the finger was last (bejeweled3-nx, 2026-10-22).
void PollSwitch() {
  static bool ready = false;
  static bool was_down = false;
  if (!ready) {
    hidInitializeTouchScreen();
    ready = true;
  }
  static float last_x = 0.0f, last_y = 0.0f;
  HidTouchScreenState state{};
  const bool down = hidGetTouchScreenStates(&state, 1) != 0 && state.count > 0;
  if (down) {
    last_x = float(state.touches[0].x) / 1280.0f;
    last_y = float(state.touches[0].y) / 720.0f;
    Push(was_down ? Phase::kMoved : Phase::kBegan, last_x, last_y);
  } else if (was_down) {
    Push(Phase::kEnded, last_x, last_y);
  }
  was_down = down;
}
#endif

}  // namespace

// SexyAppBase::SexyAppBase(): keep the app object.
extern "C" REX_FUNC(__imp__sub_8243FB90);
REX_HOOK_RAW(sub_8243FB90) {
  const uint32_t app = ctx.r3.u32;
  __imp__sub_8243FB90(ctx, base);
  g_app.store(app, std::memory_order_release);
  REXLOG_INFO("[touch] the app object is at 0x{:08X} (widget manager +{})", app, kAppWidgetManager);
}

extern "C" REX_FUNC(__imp__sub_8241E950);  // WidgetManager::GetWidgetAt(x, y, int* local_x, int* local_y)
extern "C" REX_FUNC(__imp__sub_8241EFA0);  // WidgetManager::TouchBegan(SexyTouch&)
extern "C" REX_FUNC(__imp__sub_8241F0D0);  // WidgetManager::TouchMoved(SexyTouch&)
extern "C" REX_FUNC(__imp__sub_8241F1E8);  // WidgetManager::TouchEnded(SexyTouch&)

namespace zumasrevenge::touch {

void OnWindowMouse(MouseKind kind, float x, float y) {
  switch (kind) {
    case MouseKind::kDown: Push(Phase::kBegan, x, y); break;
    case MouseKind::kMove:
      if (g_down) Push(Phase::kMoved, x, y);
      break;
    case MouseKind::kUp: Push(Phase::kEnded, x, y); break;
  }
}

void Dispatch(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(zumasrevenge_touch)) return;
  static uint64_t frame = 0;
  PollScript(++frame);
#if REX_PLATFORM_SWITCH
  PollSwitch();
#endif
  std::deque<Touch> touches;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.empty()) return;
    touches.swap(g_queue);
  }
  const uint32_t app = g_app.load(std::memory_order_acquire);
  if (!app) return;
  const uint32_t manager = Load32(base, app + kAppWidgetManager);
  if (!manager) return;
  if (REXCVAR_GET(zumasrevenge_touch_log)) {
    static bool dumped = false;
    if (!dumped) {
      dumped = true;
      std::string words;
      for (uint32_t o = 0; o < 0x100; o += 4) words += fmt::format(" {:X}:{}", o, int32_t(Load32(base, manager + o)));
      REXLOG_INFO("[touch] widget manager 0x{:08X}:{}", manager, words);
    }
  }
  // The screen in the game's coordinates: WidgetManager's mouse destination rectangle (x, y, width, height at +0xBC,
  // bejeweled3-nx), else the keys.
  int32_t left = int32_t(Load32(base, manager + 0xBC)), top = int32_t(Load32(base, manager + 0xC0));
  int32_t width = int32_t(Load32(base, manager + 0xC4)), height = int32_t(Load32(base, manager + 0xC8));
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192) {
    left = top = 0;
    width = REXCVAR_GET(zumasrevenge_touch_width);
    height = REXCVAR_GET(zumasrevenge_touch_height);
  }
  // One touch to the game: a SexyTouch in a fresh stack frame below the caller's stack pointer (above the callee's
  // frame and its back-chain / LR save words), then WidgetManager's TouchBegan / TouchMoved / TouchEnded.
  auto send = [&](Phase phase, int32_t x, int32_t y) {
    PPCContext call = ctx;
    const uint32_t sp = (ctx.r1.u32 - 0x400) & ~0xFu;
    const uint32_t touch = sp + 0x200;
    Store32(base, touch + 0, 1);  // the touch's id
    Store32(base, touch + 4, 1);  // tap count
    Store32(base, touch + 8, uint32_t(x));
    Store32(base, touch + 12, uint32_t(y));
    Store32(base, touch + 16, uint32_t(g_down ? g_last_x : x));
    Store32(base, touch + 20, uint32_t(g_down ? g_last_y : y));
    for (uint32_t o = 24; o < 64; o += 4) Store32(base, touch + o, 0);
    call.r1.u64 = sp;
    call.r3.u64 = manager;
    call.r4.u64 = touch;
    if (REXCVAR_GET(zumasrevenge_touch_log)) {
      REXLOG_INFO("[touch] {} at ({}, {})", phase == Phase::kBegan ? "began" : phase == Phase::kMoved ? "moved" : "ended",
                  x, y);
    }
    switch (phase) {
      case Phase::kBegan:
        __imp__sub_8241EFA0(call, base);
        g_down = true;
        break;
      case Phase::kMoved:
        __imp__sub_8241F0D0(call, base);
        break;
      case Phase::kEnded:
        __imp__sub_8241F1E8(call, base);
        g_down = false;
        break;
    }
    g_last_x = x;
    g_last_y = y;
  };
  // The widget under a point (WidgetManager::GetWidgetAt(x, y, int* local_x, int* local_y)).
  auto widget_at = [&](int32_t x, int32_t y, int32_t* local_x = nullptr, int32_t* local_y = nullptr) -> uint32_t {
    PPCContext probe = ctx;
    const uint32_t sp = (ctx.r1.u32 - 0x400) & ~0xFu;
    const uint32_t out = sp + 0x280;
    probe.r1.u64 = sp;
    probe.r3.u64 = manager;
    probe.r4.u64 = uint32_t(x);
    probe.r5.u64 = uint32_t(y);
    probe.r6.u64 = out;
    probe.r7.u64 = out + 4;
    __imp__sub_8241E950(probe, base);
    const uint32_t widget = probe.r3.u32;
    if (local_x) *local_x = int32_t(Load32(base, out));
    if (local_y) *local_y = int32_t(Load32(base, out + 4));
    if (widget && REXCVAR_GET(zumasrevenge_touch_log)) {
      const uint32_t vtable = Load32(base, widget);
      std::string slots;
      for (uint32_t k = 50; k <= 63; ++k) slots += fmt::format(" {}:{:08X}", k, Load32(base, vtable + k * 4));
      REXLOG_INFO("[touch] widget 0x{:08X} (vtable 0x{:08X}, local {} {}, size {}x{}):{}", widget, vtable,
                  int32_t(Load32(base, out)), int32_t(Load32(base, out + 4)), int32_t(Load32(base, widget + 0x34)),
                  int32_t(Load32(base, widget + 0x38)), slots);
    }
    return widget;
  };
  for (const Touch& t : touches) {
    const int32_t x = left + std::min<int32_t>(int32_t(t.x * float(width)), width - 1);
    const int32_t y = top + std::min<int32_t>(int32_t(t.y * float(height)), height - 1);
    if (t.phase == Phase::kBegan && g_down) continue;  // one touch at a time (bejeweled3-nx)
    if (t.phase != Phase::kBegan && !g_down) continue;  // the game never saw its start
    if (t.phase == Phase::kBegan) {
      uint32_t widget = widget_at(x, y);
      // The button prompts' layer ("A Shoot", "A Continue"): a whole-screen ButtonCalloutWidget over the board and
      // the screens, which takes no mouse input and so hid everything under it from taps. It leaves the mouse's
      // hit test (WidgetContainer's, sub_824177F8: mMouseVisible, +0x51), as the console game never uses the mouse.
      for (int k = 0; k < 4 && widget && Load32(base, widget) == kButtonCalloutVtable; ++k) {
        if (REXCVAR_GET(zumasrevenge_touch_log)) {
          const uint32_t first = Load32(base, widget + 160), last = Load32(base, widget + 164);
          for (uint32_t e = first; e && e < last && e < first + 48 * 8; e += 48) {
            std::string words;
            for (uint32_t o = 0; o < 48; o += 4) {
              const uint32_t w = Load32(base, e + o);
              float f;
              std::memcpy(&f, &w, 4);
              words += fmt::format(" {:X}:{}/{:.1f}", o, int32_t(w), f);
            }
            REXLOG_INFO("[touch] callout entry 0x{:08X}:{}", e, words);
          }
        }
        base[widget + kWidgetMouseVisible] = 0;
        if (REXCVAR_GET(zumasrevenge_touch_log)) REXLOG_INFO("[touch] callout layer 0x{:08X}: mouse-invisible", widget);
        widget = widget_at(x, y);
      }
      // A level (the Board): the touch aims the frog (the PC game's Board::MouseMove, which WidgetManager::TouchBegan
      // calls first), and the console's own shot follows: A (Board::MouseDown, the PC's click, ends early in the
      // console game's state).
      if (widget && Load32(base, widget) == kBoardVtable) {
        send(t.phase, x, y);
        // The stick too, from the frog's pivot (the Gun at +1136: its centre at +0x1DC / +0x1E0, the point the game's
        // mouse aim measures from, sub_8218C988) towards the tap: level 1-1's lesson ("HOLD LEFT AND SHOOT") waits for
        // a shot with the stick held, and the stick sets the frog's direction outright, the touch's own.
        int32_t lx = 0, ly = 0;
        widget_at(x, y, &lx, &ly);
        int16_t sx = 0, sy = 0;
        if (const uint32_t gun = Load32(base, widget + kBoardGun); gun && Load32(base, gun) == kGunVtable) {
          const float dx = float(lx) - LoadF32(base, gun + kGunCentre), dy = float(ly) - LoadF32(base, gun + kGunCentre + 4);
          const float len = std::sqrt(dx * dx + dy * dy);
          if (len >= 1.0f) {
            sx = int16_t(dx / len * 30000.0f);
            sy = int16_t(-dy / len * 30000.0f);  // the stick's Y is up, the screen's down
          }
        }
        zumasrevenge::input_script::PressStick(sx, sy, kPadA, 10, 6);
        if (REXCVAR_GET(zumasrevenge_touch_log)) {
          REXLOG_INFO("[touch] board: aimed at ({}, {}), stick ({}, {}), A", lx, ly, sx, sy);
        }
        continue;
      }
      // The title ("Press A to Continue", a LoadingScreen with no mouse input): a tap presses A.
      if (widget && Load32(base, widget) == kLoadingScreenVtable) {
        zumasrevenge::input_script::Press(kPadA, 4);
        if (REXCVAR_GET(zumasrevenge_touch_log)) REXLOG_INFO("[touch] title screen: A");
        continue;
      }
      // The main menu (no mouse input either): its selection is one bit of +228 (Spirit Temple 1; Adventure, Weekly
      // Challenge, Iron Frog, Boss Rush 2 / 4 / 8 / 16; the right column's five icons 32 ... 512; +232 remembers the
      // middle column's for LEFT / RIGHT), which its pad handler (vtable slot 69, sub_82231208) reads at A. A tap on an
      // entry selects it there and presses A; a tap beside the entries does nothing.
      if (widget && Load32(base, widget) == kMainMenuVtable) {
        int32_t lx = 0, ly = 0;
        widget_at(x, y, &lx, &ly);
        uint32_t entry = 0;
        for (const MenuEntry& e : kMainMenuEntries) {
          if (lx >= e.left && lx < e.right && ly >= e.top && ly < e.bottom) entry = e.bit;
        }
        if (entry) {
          Store32(base, widget + kMainMenuSelection, entry);
          if (entry & 0x1E) Store32(base, widget + kMainMenuSelection + 4, entry);
          zumasrevenge::input_script::Press(kPadA, 4);
        }
        if (REXCVAR_GET(zumasrevenge_touch_log)) REXLOG_INFO("[touch] main menu at ({}, {}): entry {}", lx, ly, entry);
        continue;
      }
      // Any other screen that takes no mouse input (Widget's own MouseDown, nothing under it that does) and covers the
      // screen, as the island map, the story and the level ends with their "A Continue": a tap presses A.
      if (widget && Load32(base, Load32(base, widget) + 54 * 4) == kWidgetMouseDown &&
          int32_t(Load32(base, widget + 0x34)) >= 1280 && int32_t(Load32(base, widget + 0x38)) >= 720) {
        zumasrevenge::input_script::Press(kPadA, 4);
        if (REXCVAR_GET(zumasrevenge_touch_log)) {
          REXLOG_INFO("[touch] screen 0x{:08X} (vtable 0x{:08X}) takes no taps: A", widget, Load32(base, widget));
        }
        continue;
      }
    }
    send(t.phase, x, y);
  }
}

}  // namespace zumasrevenge::touch
