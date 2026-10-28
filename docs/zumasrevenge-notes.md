# Zuma's Revenge! port: notes

This file records the port: what each step found, with its evidence. The project is a copy of zuma-nx (`94965a1`:
Zuma's port, itself a copy of feedingfrenzy-nx 1.0.0 and so of bejeweled3-nx 1.0.1), renamed zuma -> zumasrevenge with
`tools/port_rename.py`, with Renut's Direct3D layer taken from bejeweled3-nx 1.0.1 (XDK 20764: the nearest to this
game's 20871). Zuma's notes (`zuma-notes.md`), Feeding Frenzy's, Bejeweled 3's, Peggle 1's and Peggle 2's stay as the
reference for everything taken from those ports; comments that say "zuma-nx", "feedingfrenzy-nx", "bejeweled3-nx",
"peggle1-nx" or "Peggle 2" record findings from those ports.

The user's request (2026-10-27): "port zuma's revenge, you can find it in same location you found zuma, and do the same
process to it that you did for zuma + do the optimisations like you are. also test languages for both games". The
renderer is Renut on deko3d, as for Zuma.

## The game (2026-10-27)

The user's file is `D:\peggle 2\Zuma's Revenge!.rar`: one package, `Zuma's Revenge!/584111E9/000D0000/C3474230...`, an
XBLA game (content type `0x000D0000`), title **584111E9**, with no title update and no DLC. `tools/stfs_extract.py`
read its 95,278 blocks: 6 entries, 372 MiB, in `assets/game_root` (git-ignored).

- **`default.xex`** (`tools/xex_info.py`): original PE name `Zuma2 Xbox 360.exe`, base `0x82000000`, entry
  `0x82304238`. Imports `xam.xex` (112) and `xboxkrnl.exe` (287), version 2.0.20871. Static libraries: XDK **2.0.20871**
  (XAPILIB, D3D9, D3DX9, XGRAPHC, XAUDIO2, XAPOBA, XMCORE, XMP, XMIC, XONLINE, XWMAD, XNET): between Bejeweled 3's 20764
  and Peggle 2's 21256.
- **Sections:** `.rdata 82000400-82152504`, `.pdata 82152600-8217F508`, `.text 82180000-8296318C`,
  `.data 82970000-82E6ADF8`, `.XBMOVIE`, `.idata 82E70000`, `.XBLD`, `.reloc`.
- **Data:** PopCap's own framework, as Bejeweled 3 (209 RTTI types in `Sexy::`, `WidgetManager`, `ButtonWidget`, the
  dialogs): `main.rsb` (the resources), `intro.wmv` (the PopCap intro), `ZumasRevenge_Icon.png`, `AvatarAwards`,
  `ArcadeInfo.xml`.

## Code generation (2026-10-27)

- Futari's VMX keys stay on (`app/perf_overrides.toml`), as in Zuma and both Feeding Frenzy ports.
- **First run:** no "target not in any function"; four "Unresolved conditional branch to 0x828C269C" (made into fatal
  traps): the function is `0x828C2688`, a virtual method (its pointer at `0x82145ED4`, no `.pdata` entry), whose checks
  from `0x828C26AC` branch back to its early-out block; the gap fill had started a function at `0x828C26AC`. Declared
  with its size (`0xA8`, to the next `.pdata` function) in `app/overrides.toml`.
- **Jump tables** (`tools/switch_check.py --code 82180000:8296318C`): none truncated, wrong or missed (26 without a bound
  in the code, generated as switches, as in Bejeweled 3). **Pointer sweep** (`tools/pointer_fixpoint.py`): 66
  functions, fixpoint in pass 2.
- **Addresses:**

  | What | Zuma's Revenge | How |
  |---|---|---|
  | setjmp | `0x825F1FD0` | `stfd f14,0(r3); stfd f15,8(r3)` (one match); a hook word at `0x82E6ADE8` first; 12 call sites |
  | longjmp | `0x825F1CB0` | `lfd f14,0(r7)` (one match); 18 call sites |
  | memcpy / memset | `0x825EA270` / `0x825ECFB0` | Peggle 2's byte for byte (1160 / 160 bytes, the same FNV-1a 64), as Bejeweled 3's |
  | memmove | `0x825EA7A0` | Feeding Frenzy's `sub_822CEDF0` (its tail call reaches memcpy here) |
  | _stricmp | `0x825EC8F0` | Feeding Frenzy's byte for byte (88 bytes) |
  | MTRand::SRand | `sub_82543B68` | the seed 4357 and the 0x6C078965 recurrence (6 call sites): the test seed hook |
  | `_CxxThrowException` | `sub_825EAF28` | 477 call sites; raises through `sub_82606E78` |
  | `D3DDevice_Swap` | `sub_82309410` | the only `VdSwap` caller; the frame hook (Bejeweled 3's: on Swap) |
  | entry point | `0x82304238` | XEX header |

## First boots on the PC (2026-10-27, emulated GPU)

- `boot1`: 12 s in, "Call to invalid or unregistered function at guest address 0x82730C20": a whole 116-byte gap, a
  callback only reached through a pointer; `tools/runtime_gap_resolver.py --apply` declared it.
- `boot2` (90 s, A presses to 61 s): PopCap's intro video (`intro.wmv`: the WMV decoder works), the title "Zuma's
  Revenge!", the loading screen, the story's beach ("Strange frog, why do you intrude on us?"), the island map
  ("Entering Jungle"). 59 game frames a second; no unimplemented call in the log (only the framework's optional
  `properties\partner.xml` / `layout.xml` / `default*.xml`, not in the package, as in Bejeweled 3).

## The device layout (2026-10-27)

Measured on the aligned function pairs of the two Direct3D libraries (Bejeweled 3's and this game's: an anchor
alignment, `out/renut_map/anchor.txt`; same-length pairs, every displacement in the device's range): Zuma's Revenge's
device fields are Bejeweled 3's + 24 from about 11560 to 13536 (67 offsets, 342 accesses) and + 16 from 13584 on (140
offsets, 508 accesses), the rest unchanged (141 offsets). Bejeweled 3's is Peggle 2's layout 24 bytes lower from 11584
and 16 bytes lower from 13600 (bejeweled3-nx's notes), so **XDK 20871's device is Peggle 2's**. `tools/
renut_device_offsets_zr.py` (bejeweled3-nx's tool with its value columns switched) moved Renut's Direct3D layer to it:
140 lines from bejeweled3-nx's list of sites (Peggle 2's recorded values; 21 sites it recorded none for by the rule
above, which gives the +17120 bejeweled3-nx's notes name for Peggle 2) and 11 text sites from peggle2-nx's own lines
(the delta chain's private range 11008-12223 and 40 chunks, the declaration offset 0x3118, the chain ranges). The
presentation interval is +14000 (Renut's `kPresentInterval` 0x36B0; the frame hook reads it).

## Renut's hooks at Zuma's Revenge's addresses (2026-10-27)

`tools/renut_hook_remap_zr.py` (one-off, run once) moved the 326 active hook lines of Bejeweled 3's Direct3D layer:

- **222 state functions** from the two state tables ({getter, setter, default} rows, stride 12): Bejeweled 3's
  `0x82AD01E8` (101 rows) / `0x82AD06A8` (20), Zuma's Revenge's `0x82974578` / `0x82974A38`, found as the only places
  whose default columns equal Bejeweled 3's word for word; the same row and column is the same function.
- **97 API functions** by the anchor alignment of the two libraries (`out/renut_map/anchor.txt`).
- **7 by reading them side by side**, which the alignment left unpaired: Clear `8230AF08`, ClearF `8230ADD0`, Present
  `82309B78`, Swap `82309410` (the shared hook), SetSurfaceClip `8230AFC8`, SetRingBufferParameters `8231AD10`,
  EndIndexedVertices `8231B0D8`.
- The 30 lines in `#if 0` (functions Bejeweled 3 does not link, at their Peggle 2 addresses) stay as they are. None of
  those functions is linked here either: every Direct3D function the game calls (`out/renut_map/zr_d3d_calls.txt`, 149
  targets of `bl` from outside 0x82305000-0x82331000) is a hooked one, one Bejeweled 3 calls unhooked too, or a thunk
  / one-word getter (`82309408` -> SynchronizeToPresentationInterval, `823178C8`, `82308470`).
- The raw shader hooks' guest calls (`__imp__sub_823142B0` / `82314308`, GetFunction) by hand.

**Guest data**, each read at the same instruction of the aligned function as Bejeweled 3's use:

| What | Zuma's Revenge | Bejeweled 3 | Check |
|---|---|---|---|
| state tables | `0x82974578` / `0x82974A38` | `0x82AD01E8` / `0x82AD06A8` | the defaults equal |
| format table | `0x8205EAA0` | `0x82001620` | 128 bytes equal (9 uses) |
| caps | `0x82061EC0` | `0x82001830` | 304 bytes equal |
| full viewport | `0x8205E9D0` | `0x82001284` | 24 bytes equal |
| build qword | `0x82974B28` | `0x82AD07A0` | `0x0002000051874000` (20871) |
| `VdGlobalDevice` slot | `0x82000610` | `0x82000618` | holds `0xFFCAE000` (`0x8200060C`: `0xFFCAD000`) |
| HiZ owner | `0x829C4CC0` | `0x82B113E0` | 3 uses |
| export switches | `0x829C4CDC` / `0x829C4CE0` | `0x82B103B0` / `0x82B103B4` | |
| no-large-pages flag | `0x829C3C98` | `0x82B10380` | 2 uses |
| XMemFree | `sub_82605D80` | `sub_825F8670` | 90 call sites |
| GetCurrentThreadId | `sub_82605DB0` | `sub_825F8690` | 8 call sites |

## Renut on the PC (2026-10-27)

- **Shaders:** the trace build (`-DZUMASREVENGE_RENUT_RENDERER=OFF`, `app/src/zumasrevenge_d3d_trace.cpp` on
  Direct3D_CreateDevice and the shader / declaration creators) played to level 1-1 (`trace1`, 170 s): the game creates
  14 shaders, 4 of them pixel shaders not stored whole in `default.xex` (`ps_033c7dd9...`, `ps_6d814720...`,
  `ps_2ec3eb23...`, `ps_634940fe...`, each created twice). With the image's 71 (66 vertex, 5 pixel: the game's and the
  libraries') Renut's dump holds 75 (`out/renut_shader_dump`; the run's, named by XXH3, in `out/zr_shaders`).
- **`renut1`** (Vulkan, 120 s, the same input as `trace1`): the title, the story, the island map and level 1-1
  ("Shipwrecked!") played as on the emulated GPU; 60 game frames a second from the title on, Renut's render thread
  0.5-0.7 ms and the GPU 1.3-2.1 ms a frame; no shader missing, no warning.
- **The intro video** shows on the PC too (`touch9`, shots every 2 s: the PopCap logo, the flag, the storm); the
  black shots of `renut1` (6 s and 12 s) were dark moments of the video. Its frames present through
  D3DDevice_Present (`sub_82309B78`), not Swap, so they skip the frame hook's pacing: 127 a second on the PC, whose
  minimised window has no vsync (the console's swapchain paces them).

## Languages (2026-10-27)

- **The game's six:** `main.rsb` holds six string tables, named by their `<String id="LOCALE">`: English_United States,
  German_Germany, Spanish_Spain, French_France, Italian_Italy, Portuguese_Brazil. The game's only XGetLanguage call is
  the C runtime's locale set-up (`sub_82304050`), from which the framework picks its table.
- **The port's option** (`zumasrevenge_language`, from Bejeweled 3's: en, ja, de, fr, es, it, pt) is now en, de, fr, es,
  it and pt; an empty option follows the system (Japanese, Chinese, Korean and the rest give en).
- **PC runs** `lang_<xx>` (Renut, 40 s each, `REX_ZUMASREVENGE_LANGUAGE=<xx>`; `out/renut_tests/languages.jpg`): each of
  the six shows its own title prompt ("Drücke A zum Fortfahren", "Appuyer sur A pour continuer", "Pulsa A para
  continuar", "Premi A per continuare", "Aperte A para continuar") and the story's first lines in its language;
  `ja` (user_language 2) shows English, as the game has no Japanese.

## The first Switch build (2026-10-27)

- **Shaders:** XenosRecomp with `BANJO_HLSL_OVERRIDE` / `BANJO_DEKO3D_SPIRV_DIR` (docs/building.md, section 3), then
  `deko3d_shaders.py` (75 of 75 compile with uam: 66 vertex, median 35 KB, the largest 206 KB; 9 pixel of about 1 KB),
  `deko3d_glsl_opt.py --check`, `deko3d_shader_package.py` (531 KB), `renut_switch_shader_cache.py`. The NRO carries
  all 75: the image's containers are the ones the game and its libraries can create, and the 4 the run made.
- **Build:** `tools/win/build_switch.ps1 -Pgo none -Define "ZUMASREVENGE_SWITCH_LTO=OFF"` (development: no LTO, no PGO).
  NRO 38.6 MB, "Zuma's Revenge!" 0.1.0, the game's `ZumasRevenge_Icon.png` (64x64) scaled to 256x256 as the
  development icon (`out/icon_dev.jpg`; game data, not committed).
- **`tools/check_switch_build.py`:** all checks passed once Xbox360AppDriver::DoExit `sub_82555090` (it logs
  "Xbox360AppDriver::DoExit(): %d" and stops at its own `b .`) went into KNOWN_HALTS; 393 vector fused multiply-adds in
  44 functions (the generated code's `vmx_madd_ps`).
- **Ryujinx** (`emu1`, 200 s; SD `switch\zumasrevenge-nx` a junction to `out/emu_sd/zumasrevenge-nx`;
  `tools/build_asset_pack.py` made the game pack, 6 files, 372 MB; `out/zr_emu_script.txt`): **the intro video plays**
  (the PopCap logo, the flag, the storm, the frog's raft, the title's reveal), then
  the title, the story, the island map and level 1-1 played (matches, combos, a ball chain to the skull). 43.6-60.0
  game frames a second in 10 s windows (57-60 in most of the level's, 47.4 in one), Renut's render thread 1.4-2.8 ms a
  frame, no error in the log.
- **Delivered:** `out/dist/dev_20261027_zumasrevenge/zumasrevenge-nx/` (NRO `a4dd0384...`, the game pack, README.txt);
  ELF and map `out/nx/elf/zumasrevenge_dev_20261027.*`.

## Optimising the hot functions (2026-10-27, from the emulator)

The method is docs/optimising-functions.md, as in Zuma and both Feeding Frenzy ports.

- **Profile** (Ryujinx `prof1`: 330 s, level 1-1 played with `out/zr_emu_script.txt`, `switch_profile_report` /
  `switch_profile_stacks`; map `out/nx/elf/zumasrevenge_dev_20261027.map`; `tools/switch_hot_functions.py --top 5000
  --from-report 4`, all guest threads): the game's main thread is 5.1 % of the busy samples; 150 generated functions
  were sampled, 138 eligible (12 tiny leaves left out). The hottest: XMemSet `sub_826067D8` (361 samples, a game
  thread's 15 %), the XDK audio library (`sub_828BA738`, `sub_828BA0B0`, `sub_828C9308`, ...), and on the main thread
  `sub_821C5098` (105), `sub_824690B8`, `sub_8249F0B0`, `sub_8246BB78`, `sub_8249EFF8`.
- **Hand-written natives:**
  - `h_826067D8.h`, the XDK's XMemSet (r3 = dst, r4 = byte, r5 = n: a byte loop below 16, else stvlx / stvx / dcbz128
    lines / stvx / stvrx): one host memset, the copy's registers (r4-r12, v0, v12, v13, lr) in closed form per path;
    a range that wraps or straddles 0xE0000000, or a stride word (0x82981FB4) other than 16, takes the copy.
  - `h_821C5098.h`, MSVC's uninitialized copy of 36-byte elements (a guest memcpy call each): one host copy of the span
    while memcpy's native is on and the ranges are ordinary (whole elements, no 0 destination, no overlap, no wrap),
    else the copy.
- **Localized copies:** all 138 eligible (35 in `localized.txt`, 103 in `localized_bulk.txt`, 8 shards).
- **Checks:**
  - PC verify mode (`verify1`, `verify2`: Renut, level 1-1, 150 s each; 47.2 and 43.4 million guarded calls):
    XMemSet's native checked over a million calls, no difference. Two differences a run, each in the XDK audio
    library (`sub_828BA738` f13 and `sub_828BA0B0` memory in the first, `sub_828C7D58` r9 one apart and
    `sub_828BA0B0` again in the second, other values): two threads on one audio object (r3 the same in both of the
    first run's, logged in the same millisecond), the race bejeweled3-nx's notes record for `sub_828AFB28` (the guard
    runs native and original one after the other while another thread writes the object). Not reproducible, and not
    the same function twice in the same way.
  - `tools/check_switch_build.py`: all checks passed.
- **Measured** (Ryujinx, the same NRO with the 138 natives on, `prof2`, and their keys off, `prof3`; the same script and
  length): the main thread's median 3.93 -> 3.71 ms a game frame; 57.3 -> 58.9 game frames a second on average (5 -> 3
  of 25 windows below 58). XMemSet 314 -> 16 samples; the 36-byte copy 115 -> none (the others move both ways: the
  emulator's samples land on function entries).

## Touch controls (2026-10-27)

The user: "Is there symbols in the code to add touch support to zuma and zuma's revenged like bejeweled 3 ... I want all
menu items clickable and tapping in levels shoots the ball in that direction". `app/src/zumasrevenge_touch.cpp`
(bejeweled3-nx's method; `out/touch/notes.txt`, `out/touch/vtables*.txt`):

- **The framework's touch path is linked**, as in Bejeweled 3: SexyAppBase's slots 96 / 97 / 98 jump to
  WidgetManager's TouchBegan / TouchEnded / TouchMoved (`sub_8241EFA0` / `sub_8241F1E8` / `sub_8241F0D0`); the app
  object from SexyAppBase's constructor `sub_8243FB90`; touches dispatched from the Swap hook between frames. The
  widget manager's mouse rectangle is (-208, -36, 1280, 720): the game's 864x648 centred on the screen, 1:1.
- **What takes taps as it is:** the dialogs' buttons (`ZumaDialogButton`): the pause menu, Help & Options, How To Play
  (`touch13`). The console's own screens take none:
  - **the button prompts' layers** (`ButtonCalloutWidget`, one a screen, whole-screen, on top) hid every tap: each now
    leaves the mouse's hit test (its mMouseVisible, +0x51), and a tap on a prompt presses its button (the prompts at
    +160..+164, 48 bytes: +0 PopCap's button 6 A / 7 B / 8 X / 9 Y, +0x24 width, +0x28 x; the bottom row); the layers
    are found in the widget tree (the containers' std::list at +4) each tap (`touch16`, `touch17`: the help dialog's
    "B Back");
  - **the title** (`LoadingScreen`): A; **the main menu** (`MainMenu`): its selection is one bit of +228 (Spirit Temple
    1, the four modes 2-16, the five icons 32-512), which its pad handler (slot 69, `sub_82231208`) reads at A: the
    tapped entry is selected, then A; **other whole-screen widgets without mouse input** (the story, the map, a
    level's end): A.
- **A level** (`Board`): its MouseMove aims the frog at the mouse (`sub_8218C988`, from the Gun's centre at Gun
  +0x1DC / +0x1E0; the Gun at Board +1136), but its MouseDown ends early in the console game's state. A tap aims (the
  touch's own MouseMove) and the touch pad then holds the stick from the frog's centre towards the tap (the stick sets
  the frog's direction outright) and presses A while it is held (`input_script::PressStick`): level 1-1's lesson
  ("HOLD LEFT AND SHOOT") waits for a shot with the stick held, which a mouse aim alone never gave (`touch9`).
- **Checked:** the PC, touch alone (`touch11`): the title, Adventure from the main menu, the story, the map, level 1-1's
  lesson and play (combos, the score 1,200 -> 12,150). Ryujinx, the Switch build, touch alone (`touch_emu1`): the same
  path, the lesson passed, combos.
- **Not by touch:** opening the pause menu (the + button, as on the console). Not reached in the tests: the later
  levels where the frog moves between lily pads, the boss levels, the other modes' screens.
- **Delivered:** `out/dist/dev_20261027_zumasrevenge/zumasrevenge-nx/` (NRO `3307b7db...`: the optimised build with
  touch; the README with the touch controls); ELF and map `out/nx/elf/zumasrevenge_dev_touch_20261027.*`.

## Touch: the frog's aim, and hold to aim (2026-10-28)

The user: "Often when touching and sending out the ball in zuma's revenge the frog does not rotate and the ball does
not launch in the right direction", then "if you do a tap held down you can rotate the frog/aiming reticle and when you
tap again you fire" (both games).

- **Why the shots went astray** (a PC run's per-frame watch of the Gun and the Board after each tap, `aim1` / `aim2`):
  the frog's direction is Gun +0x254 (radians, 0 down the screen, a quarter turn right, 0..2 pi; +0x258 the same), and
  the game turns it a little each frame towards its aim (about 10 frames for a large turn). Past level 1-1's lesson the
  touch's own MouseDown (WidgetManager::TouchBegan's) shot 4 frames after the tap (Gun +0x13C 0 -> 2, +0x25C the reload
  from 0.07 back to 1), with the frog still on its way: tapping at 1.90 from 0.35 shot at 1.53; 0.12 -> 5.43 shot at
  5.65. The farther the turn, the worse the miss.
- **Now:** a touch in a level never reaches the game's touch path. The frog is turned outright (Gun +0x254 / +0x258)
  and the shot is A with the stick held its way (`input_script::Aim`, which also replaces an earlier tap's stick that
  is still held, so a quick second tap is not turned back): level 1-1's lesson ("HOLD LEFT AND SHOOT") waits for a
  shot with the stick held.
- **Hold to aim:** a touch longer than 12 frames (0.2 s) or one that moves more than 24 pixels is a hold: the frog
  follows the finger (the stick held its way each frame) and nothing is shot; the frog stays aimed, and the next tap on
  that level shoots where it points, wherever it lands. A tap otherwise turns the frog to it and shoots when it ends.
- **Checked** (PC, `hold1`, touch alone): the lesson passed; taps shot at their angle (the Gun's shot state 2 eight
  frames after, the frog on the tap's angle); a hold dragged to the upper right (1.897) shot nothing, and the next tap
  at the lower left shot at 1.895; a still hold (0.5 s) aimed and the next tap shot there; a hold while aimed aimed
  again. Before the change (`aim3` / `aim4`, the frog turned outright but the touch still sent): the frog on the tap's
  angle within 0.003 at the shot, against misses up to 0.37 before.
