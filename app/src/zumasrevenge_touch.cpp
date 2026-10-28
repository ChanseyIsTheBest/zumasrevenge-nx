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
//     MouseDown(x, y, 1) (Widget's defaults), the PC game's mouse handling: the dialogs' buttons (ZumaDialogButton:
//     the pause menu, Help & Options, How To Play ...) take taps as they are.
// Nothing calls those three on the console. This file calls them on the game's own thread, between frames (from the
// D3DDevice_Swap hook, zumasrevenge_frame_hooks.cpp), with the touches queued since the previous frame.
//
// The console's own screens take no mouse input, so a tap there goes the pad's way (input_script::Press, the touch
// controls' pad merged into player 1), each by its class (RTTI):
//   - the button prompts' layers (ButtonCalloutWidget, one a screen) leave the mouse's hit test, and a tap on a prompt
//     ("A Select", "B Back") presses its button;
//   - the title (LoadingScreen): A;
//   - the main menu (MainMenu): the tapped entry selected, then A;
//   - a level (Board): a tap turns the frog to it and shoots (A, the stick held there) as soon as the Gun is ready, the
//     frog held on the tap's angle until the shot left (ServeShots); a held touch turns the frog after the finger
//     without shooting, and the next tap then shoots where it points; a second finger on the screen with it (a
//     two-finger tap) is B, the balls swapped, and nothing shot;
//   - any other whole-screen widget without mouse input (the story, the island map, a level's end): A.
//
// The app object: SexyAppBase's constructor (sub_8243FB90: stores the SexyAppBase vtables 0x820B04AC / 0x820B049C at
// +0 / +4; called once, from SexyApp's sub_824207E8) is hooked to keep `this`. (sub_8243F260, which also names those
// vtables, is the destructor.)

#include "zumasrevenge_touch.h"

#include "zumasrevenge_input_script.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
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
                      "Test runs: scripted touches, comma-separated \"<frame>:<down|move|up|second>:<x>:<y>\" in "
                      "1280x720 screen pixels (second: another finger lands), frames counted by this file's dispatch "
                      "(one a swap); empty = off");

namespace {

constexpr uint32_t kAppWidgetManager = 880;  // SexyAppBase::mWidgetManager (the slot-96..98 thunks load it)
constexpr uint32_t kWidgetMouseVisible = 0x51;  // Widget::mMouseVisible (mVisible +0x50, mDisabled +0x52)
// RTTI (out/touch/notes.txt): Sexy::ButtonCalloutWidget, the button prompts' layer, and Sexy::LoadingScreen, the title.
constexpr uint32_t kButtonCalloutVtable = 0x820090DC;
constexpr uint32_t kLoadingScreenVtable = 0x82020B3C;
constexpr uint32_t kBoardVtable = 0x820032BC;  // Sexy::Board, a level
constexpr uint32_t kScreenTransitionVtable = 0x82048C74;  // Sexy::ScreenTransitionWidget, the whole screen, behind a level
constexpr uint32_t kWidgetX = 0x2C, kWidgetY = 0x30;      // Widget mX / mY (mWidth / mHeight at +0x34 / +0x38)
// The dialogs (RTTI, out/touch/vtables2.txt): PauseDialog, HelpAndOptionsDialog, OptionsDialog, ControlsDialog,
// GenericHelp, ZumaDialog, Dialog.
constexpr uint32_t kDialogVtables[] = {0x8202428C, 0x82012A7C, 0x82023E24, 0x8200932C, 0x82011AC4, 0x8204D21C, 0x820BBD3C};
constexpr uint32_t kBoardGun = 1136;         // Board's Gun (built by sub_821F2948 from Board's sub_8219DBE8)
constexpr uint32_t kGunVtable = 0x82012848;  // Sexy::Gun
constexpr uint32_t kGunCentre = 0x1DC;       // two floats, the board's coordinates
// zumasrevenge-nx (2026-10-28, a PC run's per-frame watch of the Gun after taps, `aim2`): the frog's direction in
// radians at +0x254, the same at +0x258 (0 points down the screen, a quarter turn right, 0..2 pi). The game turns it a
// little each frame towards its aim (about 10 frames for a large turn).
constexpr uint32_t kGunAngle = 0x254;
// A touch in a level that lasts longer than this (0.2 s at 60 frames a second), or moves further than this (the game's
// coordinates, 1:1 with the screen's pixels), is a hold.
constexpr uint64_t kHoldFrames = 12;
constexpr int32_t kHoldMove = 24;
// A touch turns the frog only from its kTurnDelay-th frame (a tap at its end, when it shoots), so a two-finger tap's
// first finger, landing a frame or two before the second, does not turn it for a frame (the user, 2026-10-28: "stop
// the frog from turning for a frame when doing a double tap").
constexpr uint64_t kTurnDelay = 5;
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
constexpr uint16_t kPadB = 0x2000;  // XINPUT_GAMEPAD_B: a level's ball swap
constexpr uint32_t kWidgetVisible = 0x50;  // Widget::mVisible
constexpr int32_t kPromptTop = 628, kPromptBottom = 692;  // the prompts' row, screen pixels

// PopCap's pad buttons (the main menu's handler: 0-3 the directions, 4 START, 6 A) as XInput bits.
uint16_t PadOfPrompt(uint32_t button) {
  switch (button) {
    case 6: return 0x1000;  // A
    case 7: return 0x2000;  // B
    case 8: return 0x4000;  // X
    case 9: return 0x8000;  // Y
    default: return 0;
  }
}

std::atomic<uint32_t> g_app{0};

// kSecond: another finger landed while the touch is down (the PC: the right mouse button).
enum class Phase : uint8_t { kBegan, kMoved, kEnded, kSecond };
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
inline void StoreF32(uint8_t* base, uint32_t address, float value) {
  uint32_t w;
  std::memcpy(&w, &value, 4);
  Store32(base, address, w);
}

// A touch in a level (the user, 2026-10-28: "if you do a tap held down you can rotate the frog/aiming reticle and when
// you tap again you fire"). A tap turns the frog to it and shoots when it ends. A hold turns the frog after the finger
// and does not shoot; the frog stays aimed (`g_armed_board`), and the next tap on that Board shoots where it points,
// wherever it lands (a hold then aims again). The game never sees these touches: its own touch path (TouchBegan's
// MouseDown) would shoot at once.
// A second finger (the user, 2026-10-28: "tapping with two fingers at the screen at the same time" swaps the balls) is
// B. A touch that is not a hold yet is then no tap: the frog turned back to where it pointed before it (the first
// finger may land a frame early) and nothing more; a hold goes on aiming. Each further second finger swaps again.
struct LevelTouch {
  bool active = false;   // a touch that began on the Board is down
  bool hold = false;     // it is a hold
  bool swapped = false;  // a second finger swapped the balls before it was a hold: it does nothing more
  uint64_t start = 0;    // the frame it began
  uint32_t board = 0, gun = 0;
  int32_t start_x = 0, start_y = 0, x = 0, y = 0;  // the game's coordinates (WidgetManager's)
  int32_t to_local_x = 0, to_local_y = 0;          // from those to the Board's own
  uint32_t angle_before[2] = {};                   // the Gun's +0x254 / +0x258 before the touch
};
LevelTouch g_level;
uint32_t g_armed_board = 0;
float g_armed_angle = 0.0f;  // the frog's angle when the hold ended

// The frog turned to (x, y), the game's coordinates, at once; returns its angle (unchanged when the point is on its
// pivot: the Gun's centre at +0x1DC / +0x1E0, the point the game's mouse aim measures from, sub_8218C988).
float TurnFrog(uint8_t* base, const LevelTouch& t, int32_t x, int32_t y) {
  const float dx = float(x + t.to_local_x) - LoadF32(base, t.gun + kGunCentre);
  const float dy = float(y + t.to_local_y) - LoadF32(base, t.gun + kGunCentre + 4);
  if (dx * dx + dy * dy < 1.0f) return LoadF32(base, t.gun + kGunAngle);
  float angle = std::atan2(dx, dy);
  if (angle < 0.0f) angle += 6.2831853f;
  StoreF32(base, t.gun + kGunAngle, angle);
  StoreF32(base, t.gun + kGunAngle + 4, angle);
  return angle;
}

// The touch controls' stick pointing the frog's way (the stick's Y is up, the screen's down).
void StickAlong(float angle, int16_t& sx, int16_t& sy) {
  sx = int16_t(std::sin(angle) * 30000.0f);
  sy = int16_t(-std::cos(angle) * 30000.0f);
}

bool LevelValid(const uint8_t* base, uint32_t board, uint32_t gun) {
  return Load32(base, board) == kBoardVtable && Load32(base, board + kBoardGun) == gun &&
         Load32(base, gun) == kGunVtable;
}
bool LevelTouchValid(const uint8_t* base, const LevelTouch& t) { return LevelValid(base, t.board, t.gun); }

// The first visible widget of a class in the WidgetManager's tree (each container's children: an MSVC std::list at +4,
// node +4 the previous, +8 the widget, walked from the top as the hit test does).
uint32_t FindVisible(const uint8_t* base, uint32_t manager, uint32_t vtable) {
  uint32_t found = 0;
  auto walk = [&](auto&& self, uint32_t container, int depth) -> void {
    const uint32_t head = depth < 8 ? Load32(base, container + 4) : 0;
    if (!head) return;
    uint32_t node = Load32(base, head + 4);
    for (int n = 0; node && node != head && n < 512 && !found; ++n) {
      const uint32_t w = Load32(base, node + 8);
      if (w && base[w + kWidgetVisible]) {
        if (Load32(base, w) == vtable) {
          found = w;
          return;
        }
        self(self, w, depth + 1);
      }
      node = Load32(base, node + 4);
    }
  };
  walk(walk, manager, 0);
  return found;
}
bool DialogVisible(const uint8_t* base, uint32_t manager) {
  for (const uint32_t dialog : kDialogVtables) {
    if (FindVisible(base, manager, dialog)) return true;
  }
  return false;
}

// A tap's shot (the user, 2026-10-28: Zuma's misdirected shots, "zuma's revenge also has same issue but less
// frequent"). The Gun's +0x13C is its shot's state: 0 ready, 1 the shot leaving (two frames after the tap's A began),
// 2 the next ball coming up (+0x25C its size, 0.07 to 1); a shot and its reload take 12 frames, and A meanwhile is
// ignored (`watch1`, a PC run's per-frame watch of the Gun: of six taps 8 frames apart, the three in a reload shot
// nothing). A tap's A was pressed at once: in a reload it was lost, and a touch right after it could turn the frog
// before it shot. Now each tap's shot waits for the Gun to be ready, with the frog held on the tap's angle until the
// shot left (+0x13C 2); then the next shot. At most kShotsWaiting wait (a newer tap takes the last one's place); a
// shot the game does not take within kShotTimeout frames of its A, or whose level ended or is under a dialog (the
// pause menu: A there would choose its entry), is given up.
constexpr uint32_t kGunState = 0x13C;
constexpr size_t kShotsWaiting = 2;
constexpr uint64_t kShotTimeout = 20;
struct Shot {
  uint32_t board, gun;
  float angle;
};
std::deque<Shot> g_shots;
uint64_t g_shot_pressed = 0;  // the frame the first shot's A was pressed, 0 before

void QueueShot(const Shot& shot) {
  if (g_shots.size() >= kShotsWaiting) g_shots.pop_back();
  g_shots.push_back(shot);
}

// Each frame, at the end of the dispatch (after the touches turned the frog): the first shot served.
void ServeShots(uint8_t* base, uint32_t manager, uint64_t frame, bool log) {
  while (!g_shots.empty()) {
    const Shot& s = g_shots.front();
    if (!LevelValid(base, s.board, s.gun) || !manager || DialogVisible(base, manager)) {
      if (log) REXLOG_INFO("[touch] board: {} shot(s) given up (the level ended, or a dialog is up)", g_shots.size());
      g_shots.clear();
      g_shot_pressed = 0;
      return;
    }
    const uint32_t state = Load32(base, s.gun + kGunState);
    if (g_shot_pressed) {
      const bool left = state == 2, late = frame - g_shot_pressed > kShotTimeout;
      if (left || late) {
        if (log) REXLOG_INFO("[touch] board: the shot at {} {}", s.angle, left ? "left" : "was not taken: given up");
        g_shots.pop_front();
        g_shot_pressed = 0;
        continue;
      }
    }
    StoreF32(base, s.gun + kGunAngle, s.angle);
    StoreF32(base, s.gun + kGunAngle + 4, s.angle);
    if (!g_shot_pressed && state == 0) {
      // A with the stick held its way (level 1-1's lesson, "HOLD LEFT AND SHOOT", waits for a shot with the stick held).
      int16_t sx, sy;
      StickAlong(s.angle, sx, sy);
      zumasrevenge::input_script::Aim(sx, sy, kPadA, 8, 2);
      g_shot_pressed = frame;
    }
    return;
  }
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
        const Phase phase = k == "down"     ? Phase::kBegan
                            : k == "move"   ? Phase::kMoved
                            : k == "second" ? Phase::kSecond
                                            : Phase::kEnded;
        script.push_back({when, phase, x / 1280.0f, y / 720.0f});
      } else {
        REXLOG_WARN("[touch] zumasrevenge_touch_script: \"{}\" is not <frame>:<down|move|up|second>:<x>:<y>", item);
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
// docked, nothing touches it). A release reaches the game where the finger was last (bejeweled3-nx, 2026-10-22). The
// touch is the first finger (by its id); another finger landing beside it is kSecond. Once the first finger lifts, the
// others are ignored until all have.
void PollSwitch() {
  static bool ready = false;
  if (!ready) {
    hidInitializeTouchScreen();
    ready = true;
  }
  static bool down = false, wait_all_up = false;
  static uint32_t finger = 0;
  static int32_t was_count = 0;
  static float last_x = 0.0f, last_y = 0.0f;
  HidTouchScreenState state{};
  const int32_t count = hidGetTouchScreenStates(&state, 1) ? std::clamp<int32_t>(state.count, 0, 16) : 0;
  if (down) {
    int32_t i = 0;
    while (i < count && state.touches[i].finger_id != finger) ++i;
    if (i < count) {
      last_x = float(state.touches[i].x) / 1280.0f;
      last_y = float(state.touches[i].y) / 720.0f;
      Push(Phase::kMoved, last_x, last_y);
    } else {
      Push(Phase::kEnded, last_x, last_y);
      down = false;
      wait_all_up = count > 0;
    }
  } else if (count > 0 && !wait_all_up) {
    finger = state.touches[0].finger_id;
    last_x = float(state.touches[0].x) / 1280.0f;
    last_y = float(state.touches[0].y) / 720.0f;
    Push(Phase::kBegan, last_x, last_y);
    down = true;
  }
  if (count == 0) wait_all_up = false;
  if (down && count >= 2 && was_count < 2) Push(Phase::kSecond, last_x, last_y);
  was_count = count;
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
  static bool down = false;  // the window's thread only
  switch (kind) {
    case MouseKind::kDown:
      down = true;
      Push(Phase::kBegan, x, y);
      break;
    case MouseKind::kMove:
      if (down) Push(Phase::kMoved, x, y);
      break;
    case MouseKind::kUp:
      down = false;
      Push(Phase::kEnded, x, y);
      break;
    case MouseKind::kSecond:
      if (down) Push(Phase::kSecond, x, y);  // the right button with the left held: a second finger
      break;
  }
}

void Dispatch(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(zumasrevenge_touch)) return;
  static uint64_t frame = 0;
  PollScript(++frame);
#if REX_PLATFORM_SWITCH
  PollSwitch();
#endif
  // A touch in a level, each frame: past kHoldFrames it is a hold. A hold turns the frog after the finger with the
  // stick held its way; a touch that may still be a tap turns it too from its kTurnDelay-th frame, unless a hold aimed
  // it before (the tap then shoots where it points). The level gone (its end, the next one), the touch ends. A touch
  // that swapped the balls does nothing more.
  if (g_level.active && !g_level.swapped) {
    if (!LevelTouchValid(base, g_level)) {
      g_level.active = false;
    } else {
      if (!g_level.hold && frame - g_level.start >= kHoldFrames) {
        g_level.hold = true;
        if (REXCVAR_GET(zumasrevenge_touch_log)) REXLOG_INFO("[touch] board: a hold, aiming");
      }
      if (g_level.hold || (g_armed_board != g_level.board && frame - g_level.start >= kTurnDelay)) {
        const float angle = TurnFrog(base, g_level, g_level.x, g_level.y);
        if (g_level.hold && g_shots.empty()) {  // not while a shot waits: its A goes with its own stick
          int16_t sx, sy;
          StickAlong(angle, sx, sy);
          zumasrevenge::input_script::Aim(sx, sy, 0, 3, 0);
        }
      }
    }
  }
  const uint32_t app = g_app.load(std::memory_order_acquire);
  const uint32_t manager = app ? Load32(base, app + kAppWidgetManager) : 0;
  // The shots, at the end of this dispatch however it ends (after the touches turned the frog).
  struct ServeAtEnd {
    uint8_t* base;
    uint32_t manager;
    uint64_t frame;
    ~ServeAtEnd() { ServeShots(base, manager, frame, REXCVAR_GET(zumasrevenge_touch_log)); }
  } serve_at_end{base, manager, frame};
  std::deque<Touch> touches;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_queue.empty()) return;
    touches.swap(g_queue);
  }
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
    // A second finger: in a level, B swaps the balls; anywhere else nothing (B is the menus' back).
    if (t.phase == Phase::kSecond) {
      if (g_level.active && LevelTouchValid(base, g_level)) {
        if (!g_level.hold) {
          Store32(base, g_level.gun + kGunAngle, g_level.angle_before[0]);
          Store32(base, g_level.gun + kGunAngle + 4, g_level.angle_before[1]);
          g_level.swapped = true;
        }
        zumasrevenge::input_script::Press(kPadB, 4);
        if (REXCVAR_GET(zumasrevenge_touch_log)) {
          REXLOG_INFO("[touch] board: a second finger: B (swap){}", g_level.hold ? ", the hold aims on" : "");
        }
      }
      continue;
    }
    if (g_level.active && t.phase != Phase::kBegan) {
      g_level.x = x;
      g_level.y = y;
      if (!g_level.swapped && (std::abs(x - g_level.start_x) > kHoldMove || std::abs(y - g_level.start_y) > kHoldMove)) {
        g_level.hold = true;
      }
      if (t.phase == Phase::kEnded) {
        g_level.active = false;
        if (g_level.swapped || !LevelTouchValid(base, g_level)) continue;
        if (g_level.hold) {
          // A hold: the frog stays where it points, and the next tap shoots there.
          g_armed_board = g_level.board;
          g_armed_angle = LoadF32(base, g_level.gun + kGunAngle);
          if (REXCVAR_GET(zumasrevenge_touch_log)) {
            REXLOG_INFO("[touch] board: held to ({}, {}), the frog at {}: the next tap shoots", x, y, g_armed_angle);
          }
        } else {
          // A tap: the frog turned to it (unless a hold aimed it), then its shot (ServeShots).
          const bool armed = g_armed_board == g_level.board;
          const float angle = armed ? g_armed_angle : TurnFrog(base, g_level, x, y);
          QueueShot({g_level.board, g_level.gun, angle});
          g_armed_board = 0;
          if (REXCVAR_GET(zumasrevenge_touch_log)) {
            REXLOG_INFO("[touch] board: tap at ({}, {}){}, the frog at {}: shot {} of {}", x, y,
                        armed ? " after a hold" : "", angle, g_shots.size(), kShotsWaiting);
          }
        }
      }
      continue;
    }
    if (t.phase == Phase::kBegan && (g_down || g_level.active)) continue;  // one touch at a time (bejeweled3-nx)
    if (t.phase != Phase::kBegan && !g_down) continue;  // the game never saw its start
    if (t.phase == Phase::kBegan) {
      // The button prompts' layers ("A Select", "B Back", "A Shoot" ...): a whole-screen ButtonCalloutWidget over a
      // screen, which takes no mouse input and so hid everything under it from taps. Each leaves the mouse's hit test
      // (WidgetContainer's, sub_824177F8: mMouseVisible, +0x51), as the console game never uses the mouse, and its
      // prompts are taken by their place instead: it keeps them at +160 / +164 (begin / end, 48 bytes each: +0 PopCap's
      // pad button, 6 A, 7 B, 8 X, 9 Y; +4 the label; +0x24 the width and +0x28 the left edge, in screen pixels), drawn
      // on one row at the bottom (y 637-680 on every screen seen). The layers are found in the widget tree (each
      // container's children: an MSVC std::list at +4, node +4 the previous, +8 the widget, walked from the top as the
      // hit test does), the visible ones only, each screen having its own.
      const int32_t screen_x = x - left, screen_y = y - top;
      std::vector<uint32_t> callouts;
      auto find_callouts = [&](auto&& self, uint32_t container, int depth) -> void {
        const uint32_t head = depth < 8 ? Load32(base, container + 4) : 0;
        if (!head) return;
        uint32_t node = Load32(base, head + 4);
        for (int n = 0; node && node != head && n < 512; ++n) {
          const uint32_t w = Load32(base, node + 8);
          if (w && base[w + kWidgetVisible]) {
            if (Load32(base, w) == kButtonCalloutVtable) callouts.push_back(w);
            self(self, w, depth + 1);
          }
          node = Load32(base, node + 4);
        }
      };
      find_callouts(find_callouts, manager, 0);
      uint16_t prompt = 0;
      for (const uint32_t c : callouts) {
        if (base[c + kWidgetMouseVisible]) {
          base[c + kWidgetMouseVisible] = 0;
          if (REXCVAR_GET(zumasrevenge_touch_log)) REXLOG_INFO("[touch] callout layer 0x{:08X}: mouse-invisible", c);
        }
        if (prompt || screen_y < kPromptTop || screen_y >= kPromptBottom) continue;
        const uint32_t first = Load32(base, c + 160), last = Load32(base, c + 164);
        for (uint32_t e = first; e && e < last && e < first + 48 * 8; e += 48) {
          const int32_t px = int32_t(Load32(base, e + 0x28)), pw = int32_t(Load32(base, e + 0x24));
          if (screen_x >= px && screen_x < px + pw) prompt = PadOfPrompt(Load32(base, e));
        }
      }
      if (prompt) {
        zumasrevenge::input_script::Press(prompt, 4);
        if (REXCVAR_GET(zumasrevenge_touch_log)) {
          REXLOG_INFO("[touch] prompt at ({}, {}): pad {:04X}", screen_x, screen_y, prompt);
        }
        continue;
      }
      const uint32_t widget = widget_at(x, y);
      // A level (the Board): a touch of its own (LevelTouch above). The game turns the frog towards its aim a little
      // each frame (about 10 frames for a large turn), and the touch's own MouseDown (WidgetManager::TouchBegan's) shot
      // 4 frames after the tap, the ball leaving on the way (the user, 2026-10-28: "the frog does not rotate and the
      // ball does not launch in the right direction"): the frog is turned outright instead, and the shot is A.
      // The Board is the game's 864x648 in the middle of the screen, and the level's picture fills the screen around
      // it: a tap there, outside the Board, lands on the ScreenTransitionWidget behind it (the user, 2026-10-28: the
      // frog sometimes "doesn't turn"; `dense1`: every tap at x 200 or 1100 shot without turning, the transition
      // widget taking no taps and so getting A). With a level on the screen, such a tap is the level's too.
      uint32_t board = 0;
      if (widget && Load32(base, widget) == kBoardVtable) {
        board = widget;
      } else if (widget && Load32(base, widget) == kScreenTransitionVtable) {
        board = FindVisible(base, manager, kBoardVtable);
      }
      // Not under a dialog (the pause menu, Help & Options ...: A there would choose its entry).
      if (board && DialogVisible(base, manager)) board = 0;
      const uint32_t gun = board ? Load32(base, board + kBoardGun) : 0;
      if (board && gun && Load32(base, gun) == kGunVtable) {
        // The Board's own coordinates: the point less its place (Widget mX / mY, +0x2C / +0x30; the WidgetManager's
        // coordinates are the screen's less the mouse rectangle's corner).
        const int32_t to_local_x = -int32_t(Load32(base, board + kWidgetX));
        const int32_t to_local_y = -int32_t(Load32(base, board + kWidgetY));
        g_level = {true, false, false, frame, board, gun, x, y, x, y, to_local_x, to_local_y,
                   {Load32(base, gun + kGunAngle), Load32(base, gun + kGunAngle + 4)}};
        if (g_armed_board != board) g_armed_board = 0;  // a hold on another level aims nothing here
        if (REXCVAR_GET(zumasrevenge_touch_log)) {
          REXLOG_INFO("[touch] board: touch at ({}, {}){}{}", x + to_local_x, y + to_local_y,
                      board == widget ? "" : " (beside the Board)", g_armed_board ? " (aimed by a hold)" : "");
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
