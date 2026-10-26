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
