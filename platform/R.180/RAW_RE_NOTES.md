# EOS R (1.8.0) — RAW pipeline reverse-engineering notes

Working reference for bringing up sensor-RAW capture on the EOS R (DIGIC 8, Cortex-A, MMU).
Goal: ML control of the RAW pipeline — turn it on, read it, save it — toward 14-bit / 4K RAW video.

ROM base 0xE0000000. All function/register addresses are for **R firmware 1.8.0**.

---
## 1. EDMAC architecture

- **DmacInfo table @ `0xE0DD5C64`** — 8-byte entries `{pBlock, ModeInfo}`, ~76 channels. `pBlock` is the
  channel's MMIO register base (`0xD04xxxxx`, 0x100 stride). The table base is also reachable via the
  literal `*(0xE0536D58) == 0xE0DD5C64` and `*(0xE05366D8) == 0xE0DD5C64`.
- **ModeInfo bits:** bit0 = WRITE channel (DMA → RAM), bit1 = READ; bit19 (`0x80000`) = "imaging-class".
- **Channel register offsets** (within pBlock):
  - `+0xa0` = **ram_addr** (the buffer pointer; the thing we care about) — set per-frame by `FUN_e05364b6`.
  - `+0x48` = ys_xs, `+0x4c` = ya_xa, `+0x50` = yb_xb, `+0x54` = yn_xn `(yn<<16)|xn` — geometry.
  - `+0xb4` = channel enable: **1 = write-start, 2 = read-start, 0 = stop**.
  - `+0xa4/+0xa8/+0xac` = aux (small per-frame index values, NOT the buffer).
- The pixel **format/bit-depth is NOT in the EDMAC registers** — those encode geometry + bus width
  (`INFO_128/64/32BIT_MODE`). Format must be read from the buffer data pattern.

### Channel map (idx → pBlock, partial; imaging WRITE channels)
```
idx1-8  0xD0420000..0700   (front-end / processed planes; idx8=stats)
idx11   0xD0420a00         (accumulator/stats)
idx14   0xD0440000 (P14)
idx17/18 0xD045E000/100
idx23/24 0xD0487000/100    *** registers HARD-FAULT on CPU read (see gotchas) ***
idx26-31 0xD04A2000..2500  *** same faulting region ***
idx36/37 0xD04C1000/100
```

---
## 2. EDMAC API (Canon functions, for the slurp / hooks)

| Function | Addr | Notes |
|---|---|---|
| `SetEDmac(port, addr, b14, info)` | `0xE0536ABC` | geometry + addr → pBlock |
| `ConnectWriteEDmac(port, conn)` | `0xE053607C` | asserts WRITE; routes via DmacBoomerInfo |
| `ConnectReadEDmac(port, conn)` | `0xE05360C8` | asserts READ |
| `StartEDmac (WRITE)` `FUN_e053595e` | `0xE053595E` | sets +8=0x12, DSB, +0xb4=1 |
| `StartEDmac (READ)` `FUN_e0535998` | `0xE0535998` | +0xb4=2; `Port<0x4b` assert |
| stop (+0xb4=0) | `0xE0535F76` / `0xE0536142` | |
| `FUN_e05364b6(chan, addr)` | `0xE05364B6` | the per-frame **+0xa0 ram_addr setter** (hook target) |
| `FUN_e05329b0(boomerId, vdkick)` | — | `Engine::BoomerVdKick.c`; ConnectWriteEDmac sets the channel's **VdKick** (frame-sync group 0..0x18), not a data source |
| `_engio_write(reg_list)` | `0xE06B92B4` | register write primitive. `_EngDrvOut` not in ROM → wrap: `{reg,val,0xffffffff}` → `_engio_write`. On the R, SKIP the `MEM(0xC0400008)&0x2` LCLK check (all D678X cams) |
| DmacBoomerInfo table ptr | `0xE05361F8` | stride 0xc, BoomerID per channel |

---
## 3. The RAW pipeline — what we learned

- **`Warp::DprawHeadToRaw`** is the sensor-HEAD → raw ("Mem1") path. String
  `"Mem1 is Not Complete(HeadToRaw=%08x)"` @ `0xE069D03C`/`0xE064D580`.
  Enable chain: record evt → `FUN_e0704766` → `FUN_e07042c0` (DprawCorrection) →
  `FUN_e071f574` ("DprawSap_Start") → `FUN_e071f288` (DprawHeadToRaw) →
  `FUN_e06d6bda` programs chan `*(DAT_e06d6c48+0x34)` buffer via `FUN_e05364b6`.
  (`DAT_e06d6c48 == 0x000243ec`, a low-RAM config struct; chan idx is runtime.)
- The 200D / 6D2 (DIGIC-7 cousins) define `RAW_LV_EDMAC_CHANNEL_ADDR 0xd0058000 // channel 24 (index 23)`
  found "via Mem1 is Not Complete". **That precedent does NOT hold for the R** — see below.

### Empirical results (runtime hook on `FUN_e05364b6` during H264 movie recording)
The hook logged every channel whose `+0xa0` buffer is set per frame, with the buffer address (captured
from the function ARG `r1` — so even faulting-region channels are covered). Findings:
- **idx23 (`0xD0487000`) is NEVER called** during H264 movie → it is NOT the movie raw channel (the
  cousin index-23 precedent is wrong for the R; the DprawHeadToRaw/Mem1 path is not active in plain H264).
- The ~28 logged channels are **all PROCESSED planes**, none Bayer:
  - debayered previews — idx4 (8-bit, ~1132w), idx5/6/64/65/69 (8/16-bit, smooth; pixel Bayer-ratio ≈ 0.9)
  - stats/AF — idx8/11/27/66/70 (small ~320-byte strides)
  - structured — idx19 (constant `0x18` byte pattern)
- **Conclusion:** during plain H264 movie the 14-bit Bayer RAW is **not parked in a readable buffer** —
  the sensor frontend debayers it upstream. So you cannot get it by reading/redirecting a `FUN_e05364b6`
  channel. It must be **slurped from the sensor raw SOURCE** (the mlv_lite method):
  `ConnectWriteEDmac(free_write_chan, conn)` + `SetEDmac(chan, OUR_buf, geom)` + `StartEDmac(chan)`,
  run while the sensor pipeline is hot (recording). ← current work.

---
## 4. Runtime ROM-patch / hook infrastructure (VALIDATED ON HARDWARE)

- `patch_hook_function` is NOT compiled under `CONFIG_MMU_REMAP`. Use `function_hook_patch`
  (`struct {uint32_t patch_addr; uint8_t orig_content[8]; uint32_t target_function_addr;}`) +
  `convert_f_patch_to_patch()` + `apply_patches()` (all linked on the R). The patch overwrites the
  function entry with `LDR.W PC,[PC,#0]` (`f8df f000`, +2 to byte[2] if `patch_addr & 0x2`) + the target
  addr → the hook **replaces** the function (re-implement the original inside it).
- **MMU 2-page bump** (`patch_mmu.c`: `generic_mmu_space[2*MMU_PAGE_SIZE + ...]`) is needed so a runtime
  `apply_patches` can remap a 2nd 64KB ROM page. The R already remaps one page at boot
  (`mmu_patches.h` `early_code_patches`: `0xE03C10C4`); the hook target `0xE05364B6` is page `0xE0530000`.
  `MMU_PAGE_SIZE = 0x10000`; `num_64k_pages = aligned_space / MMU_PAGE_SIZE` (auto-derives from the size).
- Install hooks **at runtime from a menu**, NOT at boot → a card pull always recovers.

---
## 5. CRITICAL GOTCHAS (each cost a crash / no-boot)

- **`0xD0487xxx` / `0xD04A2xxx` channel registers HARD-FAULT on direct CPU read** — in idle LV *and*
  during recording. Only the DMA engine accesses them. Never `*(volatile*)0xD0487...`. (Get the buffer
  addr from the `FUN_e05364b6` arg instead — it's a normal RAM address you CAN read.)
- **`PROP_MVR_REC_START` un-deny CRASH-LOOPS boot.** It's in `property_whitelist.h prop_handler_deny[]`
  for a reason; delivering it to the handler at boot faults. `__recording`/`RECORDING` therefore stay 0
  on the R — don't rely on them for movie-state gating.
- **qemu cannot validate the R past early boot.** ML reaches `K424 READY` then loops on
  `[WINSYS] ERROR XCM_GetSourceSurface Illegal handle` (compositor not emulated). So qemu validates
  early boot (incl. `mmu_init` / the MMU bump) but NOT late boot, property delivery, or LV. Never trust a
  bare qemu pass for boot/property/LV-affecting changes — prefer menu-invoked code.
- **Red-LED budget:** keep `_bss_end < 0x144800` (an aperture build red-LED'd at ~`0x144900`). The MMU
  2-page bump adds +64KB BSS; offset it by removing dead code.

---
## 6. Tooling (in this tree + companion scripts)

- `src/debug.c` Debug menu: **"Arm raw hook"** (`rawhk_task`) — runtime-hooks `FUN_e05364b6`, logs
  idx/pblock/hits/a0 → `ML/LOGS/RAWHK.TXT`, and snapshots 128KB of each channel's buffer →
  `ML/LOGS/RAWHKB.BIN` (16-byte header/chunk: magic `0x52415748`, idx, a0, len). PC-side: split by magic,
  render each, find the coherent full-res Bayer frame.
- PC render helpers live alongside the dumps (stride autocorrelation + 8/14/16-bit interpretations +
  pixel-level Bayer test: raw Bayer shows adjacent≫alternate pixel diff).

---
## 7. The slurp — sensor raw source + geometry (CONFIRMED via qemu-eos `engine.c`)

The qemu-eos EDMAC model is explicit about the sensor-raw source connection:
```c
/* engine.c, write path (image-processing module -> memory) */
if (conn == 0 || conn == 35) {            /* sensor data */
    int raw_width  = xb * 8/14;           /* xb = the SetEDmac byte pitch */
    int raw_height = yb ? yb + 1 : xn + 1;
    /* transfer_data_size == raw_width * raw_height * 14/8  -> 14-BIT PACKED */
    load_fullres_14bit_raw(buf, raw_width, raw_height);
}
```
So **connection 0 (fallback 35) = the sensor 14-bit raw source**, matching mlv_lite's
`ConnectWriteEDmac(raw_write_chan, 0)`. The slurp recipe for the R:
```
ConnectWriteEDmac(free_chan, 0);                         // 0xE053607C ; conn 0 = sensor raw
SetEDmac(free_chan, OUR_buf, &{ xb = width*14/8, yb = height-1 }, dmaFlags);   // 0xE0536ABC, 14-bit packed
StartEDmac(free_chan, 0);                                // 0xE053595E ; +0xb4 = 1 (write start)
```
`raw_width = xb*8/14`, `raw_height = yb+1`, buffer = `width*height*14/8` bytes. Run while the sensor
pipeline is hot (recording). ConnectWriteEDmac asserts the channel has a valid BoomerID
(`DmacBoomerInfo[port].BoomerID != -1`, table ptr `0xE05361F8` stride 0xc) and is a WRITE channel.

### Free write channels for the slurp (DmacBoomerInfo @ `0xE0DD608C`, stride 0xc)
BoomerID (word0 of each entry) = `boomer_index << 16`; `0xFFFFFFFF` = not connectable (idx0, idx32, idx39).
Channels FREE (not in the `FUN_e05364b6` hook log) + WRITE + connectable → **slurp candidates:
idx1, idx3, idx7, idx9, idx10, idx12, idx13** (also idx15/16/20/22/25/33-38). Used-by-Canon (avoid):
idx2/4/5/6/8/11/18/19/27/30/31. **Start with idx7** (free, mid-range), try others if it doesn't carry raw.

Remaining open variables: the actual **width/height** (guess a common LV-raw res first, e.g. 1920×1080 or
the EOS R 1736-ish; the qemu geometry formula tells us xb/yb), and whether a parallel connection to conn 0
disrupts Canon's recording (mlv_lite reads conn 0 in parallel, so it should be OK — verify by camera test).
Iterate channel/conn/geometry until a coherent Bayer frame lands (pixel Bayer test: adjacent >> alternate).

### SetEDmac geometry struct (decompiled `FUN_e0536abc`) + the slurp build
`SetEDmac(port, addr, b14, edmac_info* ei)` writes geometry from `ei` (a uint array). Mapping:
`+0x48 ys_xs = ei[0xe]|ei[0x11]<<16`, `+0x4c ya_xa = ei[0xf]|ei[0x12]<<16`,
`+0x50 yb_xb = ei[0x10]|ei[0x13]<<16`, `+0x54 yn_xn = ei[0x14]|ei[0x15]<<16`.
=> **xb=ei[0x10], yb=ei[0x13], xn=ei[0x14], yn=ei[0x15]** (xs/xa/ys/ya at 0xe/0xf/0x11/0x12). With
xn=yn=xs=ys=0 the top asserts (0x4d1/0x4d5) are skipped → a single contiguous block of xb bytes ×
(yb+1) rows. addr→+0xa4, b14→+0xa8 (the BUFFER is +0xa0, set separately by FUN_e05364b6).

**"Slurp raw" task built (debug.c, md5 03f0ef1b @ 12:29):** chan idx7, conn 0, 1920×1080 guess
(pitch=W*14/8=3360, ei[0x10]=3360, ei[0x13]=1079). Seq during recording: FUN_e05364b6(7, ubuf) →
SetEDmac(7,0,0,ei) → ConnectWriteEDmac(7,0) → StartEDmac(7) → wait 400ms → stop(+0xb4=0) → copy
ubuf→SLURP.BIN (ubuf = buf|0x40000000 uncacheable). Logs idx7 regs → SLURP.TXT. EXPERIMENTAL — iterate
chan/conn/geometry. NEXT: user records + runs it; render SLURP.BIN 14-bit at width 1920 + Bayer test.

### Slurp attempt #1 result: Err 70 (Canon recording malfunction)
First "Slurp raw" (idx7, conn0, 1920x1080 guess) -> **Err 70 ~4-5s into recording**, right when the EDMAC
setup runs. So the one-shot slurp setup DISRUPTS Canon's H264 recording. Err 70 is recoverable (not a
brick). Unknown which op (setbuf/setedmac/connw/start) is the culprit. setbuf (FUN_e05364b6 +0xa0) is
proven-safe (the hook did it 12k times) -> suspect connw (re-routes the Boomer to conn 0, which Canon is
actively using) or start (conflicting transfer), or a geometry mismatch (qemu asserts geom==transfer size;
our 1920x1080 is a guess). NEXT (md5 920afc3c): STAGED slurp -- writes "last step reached: X" to SLURP.TXT
(truncating) + NotifyBox before each op, so the card pinpoints the disrupting step even if Err 70 aborts.
Likely real fix = the mlv_lite way: frame-SYNCED slurp via an LV vsync hook (raw_lv_vsync), not a one-shot
mid-frame setup; and/or ML's own raw LV (not during Canon H264). Iterating.

## 8. The ROBUST path: frame-synced slurp via the EVF state-object hook (mlv_lite's real method)
Err 70 came from a ONE-SHOT slurp setup mid-frame. mlv_lite never does that -- it slurps from a per-frame
**vsync hook**, synced to the sensor readout. ML already has the whole mechanism (src/state-object.c):
- `state_init` (INIT_FUNC) installs `stateobj_lv_spy` on `EVF_STATE` via `stateobj_start_spy` (swaps the
  state object's `StateTransition_maybe` for our spy, keeping the original).
- The spy runs on every transition; on the readout-done transition (`EVF_STATE && input==5 && old_state==5`,
  "evfReadOutDoneInterrupt") under `CONFIG_EVF_STATE_SYNC` it calls `vsync_func()` -> `raw_lv_vsync()`
  (raw.c:2129) -> `edmac_raw_slurp()` (edmac-memcpy.c:399). THAT is the frame-synced slurp.

**Status on the R: DISABLED.** `platform/R.180/internals.h:30` has `//#define CONFIG_STATE_OBJECT_HOOKS`
(commented out); `CONFIG_EVF_STATE_SYNC` and `CONFIG_EDMAC_RAW_SLURP` are not defined. But the pointer IS
ported: `platform/R.180/include/platform/state-object.h` -> `EVF_STATE = *(struct state_object**)0x77c4`.
ROM has `"EVF_STATE"` @ `0xE0056324` (name, from `"LiveView::EvfState.c"` @ 0xE0056268) and a separate
`"EVFDEV_STATE"`/`"LiveViewDrive::EvfState.c"`. So 0x77c4 should hold the LiveView EvfState object.

**Enablement plan (the robust slurp, boot-affecting -> do carefully):**
1. RUNTIME-verify the readout-done transition first (menu-invoked spy, install/log/uninstall like rawhk --
   NOT a boot change): hook `EVF_STATE->StateTransition`, log (input,old_state,new_state) for a few seconds,
   find the transition that fires exactly once/frame at the sensor readout. Confirms input/old_state (the R
   DIGIC8 EVF matrix may differ from the DIGIC-V `5/5` convention).
2. Then enable `CONFIG_STATE_OBJECT_HOOKS` + `CONFIG_EVF_STATE_SYNC` (+ the verified transition) +
   `CONFIG_EDMAC_RAW_SLURP`, wire `edmac_raw_slurp` to our free channel/conn0/geometry. The slurp then runs
   frame-synced -> should NOT trip Err 70 (it's exactly when Canon expects the channel active).
3. Gate it behind a menu flag so boot stays clean if the transition guess is wrong.
GATING DECISION: wait for the staged slurp result (which op trips Err 70). If `start` -> frame-sync is the
fix (this path). If `connw` -> the conn0 tap itself conflicts (need a different conn/chan). If `setedmac`
-> geometry. The transition logger (step 1) is the next camera build either way (characterizes LV timing).

### Slurp diagnostic result: Err 70 is at the `start` step -> frame-sync is the fix (CONFIRMED)
Staged slurp reported "last step reached: start". So setbuf + setedmac + connw ALL succeed (no Err 70):
**the geometry/edmac_info config is valid, and ConnectWriteEDmac(idx7, 0) tapping the sensor raw source
does NOT conflict with Canon.** Only StartEDmac (FUN_e053595e: +8=0x12, DSB, +0xb4=1) trips Err 70 --
starting the transfer mid-frame breaks the engine's per-frame consistency. => the slurp must be STARTED
from the per-frame readout transition (the vsync hook), exactly like mlv_lite's raw_lv_vsync ->
edmac_raw_slurp (re-armed every frame). This is the robust path (sec 8). Also noted from edmac_raw_slurp:
it RegisterEDmacComplete/Abort/PopCBR before starting, and raw_lv_vsync sets RAW_TYPE_REGISTER every frame.

### Built: "Log EVF xitions" (evflog_task) -- find the readout/vsync transition (md5 e4edaeba @ now)
Menu task: runtime-swaps EVF_STATE(0x77c4)->StateTransition_maybe(+0x0c) for a spy that tallies
(input,old_state,new_state) for 4s in LiveView, then restores (DATA-ptr swap, recoverable). Also logs the
EVF object identity (type/name/max_inputs/max_states). -> ML/LOGS/EVFLOG.TXT. The transition whose count ~=
the LV frame count (~120 over 4s @30fps) is the readout-done (vsync) point. NEXT: user runs it in LV; read
EVFLOG.TXT -> that (input,old_state) is the R's CONFIG_EVF_STATE_SYNC transition for the frame-synced slurp.

### Built (prepped, not yet deployed): "Sync slurp" (syncslurp_task) -- the frame-synced capture
The robust capture, ready to arm once EVFLOG gives the readout transition. Menu task (LiveView): installs a
runtime EVF spy (reuses evflog infra, recoverable) that, on the readout transition (SS_IN/SS_OLD --
PLACEHOLDERS 5/5, set from EVFLOG.TXT), re-arms the slurp every frame: setbuf(idx7,ubuf) + SetEDmac(geom) +
ConnectWriteEDmac(idx7,0) + StartEDmac(idx7). Starting AT the readout point (not mid-frame) is what should
dodge Err 70. After ~2s, restores Canon's handler, copies ubuf -> ML/LOGS/SLURP.BIN (+ SLURPS.TXT regs).
Build verified (_bss_end=0x142d00 < 0x144800). DEPLOY ONLY after setting SS_IN/SS_OLD from EVFLOG. Open
questions to resolve from the first capture: (a) does conn0 carry Bayer raw in plain LV, or do we need a
RAW_TYPE_REGISTER write per frame (raw.c:2137) to force raw output; (b) real geometry (1920x1080 is a guess
-- render will tell). Workflow: user runs "Log EVF xitions" -> I set the transition -> deploy "Sync slurp".

### Fallback RE status (RAW_TYPE register + geometry) -- DEFERRED until the first Sync-slurp capture
RAW_TYPE_REGISTER is only defined in raw.c for DIGIC IV (0xC0F08114) / V (0xC0F37014); the R is DIGIC 8 ->
not covered. ROM has no "RawType"/"AfRaw"/"SetCcdRaw" strings (D8 names it differently), so finding the R's
raw-type register would need decompiling the LV raw-config chain (the 5D3 route: decompile lv_af_raw ->
lv_set_raw_type -> the C0Fxxxxx reg it writes). DEFERRED on purpose: the first "Sync slurp" capture answers
whether we even need it -- if conn0 already carries 14-bit Bayer in LV, no RAW_TYPE write is needed; if the
capture is debayered/8-bit, THEN RE lv raw-type. Likewise the real LV raw geometry: the rendered first
capture reveals the true width (autocorrelation/structure), so 1920x1080 is just a starting guess. Net:
unblock by capturing first, then tune RAW_TYPE/geometry from the actual data. Waiting on EVFLOG.TXT.

### EVFLOG result (movie-mode LiveView, 2026-06-20 13:13) -- 0x77c4 VALID, readout = input 5
"Log EVF xitions" output:
```
EVF @00de8c58 type=StateObject name=EvfState inputs=16 states=8 cur=5
in=6 old=5 new=5 : 120
in=3 old=5 new=5 : 120
in=4 old=5 new=5 : 120
in=5 old=5 new=5 : 120
```
EVF_STATE(0x77c4) -> a real "EvfState" StateObject @0xde8c58 (16 inputs, 8 states). In steady LV (state 5)
each frame fires 4 inputs (3,4,5,6), all old=5->new=5, each 120x over 4s = **30fps**. **input=5,old=5 =
evfReadOutDoneInterrupt** (matches ML's DIGIC-V CONFIG_EVF_STATE_SYNC convention). => SS_IN=5/SS_OLD=5
(already the placeholder) is correct for the frame-synced slurp. Fallbacks if 5 doesn't yield raw: 6, 4, 3.
=> Deploying "Sync slurp" with the EVF spy arming at (input 5, old 5).

## 9. PIVOT: EVF hook is a known crash; use Canon's raw-LV eventprocs instead
"Sync slurp" (EVF spy arming the slurp at readout input5) -> **Err 70 + REBOOT**. Two findings:
1. **internals.h:29-30: `/* hooking EVF_STATE ends with EvfCap crashes, requires investigation */
   //#define CONFIG_STATE_OBJECT_HOOKS`** -- the R porter ALREADY found hooking EVF_STATE crashes (EvfCap).
   That IS our reboot. => the EVF-hook / CONFIG_EVF_STATE_SYNC frame-synced-slurp path is a DEAD END on R.
2. Err 70 at StartEDmac is NOT timing -- it's that the LV pipeline wasn't in RAW mode, so conn0 carried
   processed/YUV and a 2nd writer conflicted. **On D8, raw LV must be ENABLED via Canon eventprocs**
   (raw.c raw_lv_enable): `call("lv_set_mm", 1)` (D8: select RAW; lv_save_raw is YUV by default) +
   `call("lv_save_raw", 1)`. CONFIG_DIGIC_VIII is set for the R; the working D8 ports (M50/850D/M6II) use this.

**New approach "Raw-LV hook" (rawlv_hook_task, md5 c53d5860 @ 13:26):** the SAFE raw-buffer finder. In movie
LiveView (NO record): apply the proven +0xa0 hook (FUN_e05364b6), THEN call lv_set_mm(1)+lv_save_raw(1) to
make Canon output raw, log/snapshot which channel now gets the raw buffer (10s), then lv_save_raw(0) +
unpatch. No EVF hook (no EvfCap crash), no channel commandeer (no Err70) -- just enable Canon's own raw +
read the buffer it programs. -> ML/LOGS/RAWHK.TXT + RAWHKB.BIN. NEXT: user runs it in movie LV; render the
snapshot -> the channel whose buffer is now Bayer (vs the debayered ones seen without raw mode) = the RAW.

## 10. Raw-LV hook WORKED -- raw mode engaged, 24 channels; full-frame dump next
"Raw-LV hook" (lv_set_mm+lv_save_raw + the +0xa0 hook in movie LiveView, NO record) ran clean (no crash,
no Err70) and caught **24 channels** (total=9381) -- a much richer set than the debayered-only run. So the
Canon raw-LV eventprocs DO engage on the R. Hook entry read back f002f8df (the +2 LDR.W variant -- correct
for the odd-2 addr). Many channels now have rotating uncached 0x4xxxxxxx / 0x7xxxxxxx DMA buffers.

Analysis of the 128KB top-slivers (eosr_port/render_rawhkb.py, pure-stdlib): row-correlation + stride.
**14-bit-plausible candidates (stride = width*14/8):**
- idx2 & idx48: stride 6720 = **3840px @ 14-bit = 4K width** (a0 658a0000, rowcorr 0.63)
- idx45 & idx4 & idx65: stride 3360 = 1920px @ 14-bit = 1080p (rowcorr 0.51-0.59)
- idx55: stride 3584 = 2048px @ 14-bit ; idx18: stride 3192 = 1824px @ 14-bit
High byte-level adjacent/alternate (likely 16-bit interleave): idx5/6/54 (bayer 4-5.5).
**Inconclusive from slivers**: only the top ~19-39 rows captured (may be optical-black/top), and 14-bit
byte view shows packing-stripe artifacts. NEED full frames. Built "Raw-LV dump" (rawlv_dump_task, md5
5e999e78 @ 13:57): same as Raw-LV hook but also dumps FULL 4MB buffers of idx 2,45,55,5,18 -> ML/LOGS/
RWxx.BIN (~20MB). NEXT: user runs it in movie LV; render full frames (14-bit @ matching width + 16-bit for
idx5) -> the coherent Bayer scene = THE RAW + its channel + true geometry.

NOTE (strategy): for 4K RAW VIDEO the sensor-raw-in-movie-mode IS the source (can't disable the sensor);
what gets suppressed in real raw recording is the PREVIEW/display (bandwidth), not the sensor. This LV work
finds the tap point that recording reuses. (Full-res raw STILLS would be a different, higher-res readout.)

### First full dumps (idx2/45/55/5/18) -- inconclusive, but SCENE CONFIRMED in idx30
Rendered the 5 full 4MB dumps (eosr_port/render_full.py + idx2_sweep.py, multi bit-depth + contrast
stretch). None was a clean Bayer scene: idx5=mostly black (sparse/stats), idx2(4K)=smooth horizontal
streaks at ALL bit-depths (8/12/14MSB/14LSB/16), idx45/55=structured (header rows + black bands), idx18=
noise+brightness step. KEY: the 128KB sliver of **idx30 (d04a2400, stride~1920) shows a RECOGNIZABLE SCENE
-- a room interior (microwave/appliance, shelves), repeated ~3x so true width ~640.** So (a) the scene had
plenty of detail, (b) idx2's smoothness = we dumped the WRONG channels. I had picked by 14-bit-stride match,
but the most image-like channels by row-correlation were idx19/64 (0.99), idx69 (0.97), idx30 (0.93) --
NONE dumped. Also the widest buffers (idx2 4K = 14.5MB) likely TEAR during the 4MB read-while-writing,
whereas small buffers (idx30) read stable. RE-TARGETED "Raw-LV dump" -> cand {2,30,19,64,69,8} (md5
5f902e3c @ 14:40). NOTE: dump reads a0 from the hook ARG (RAM), so faulting-register channels (d04a2:
30/64/69) ARE dumpable. NEXT: user re-runs in movie LV pointing at the (detailed) scene; render all ->
the channel showing the scene with Bayer mosaic = THE RAW. (If wide ones still tear, add a per-channel
freeze: stop +0xb4 on non-faulting candidates before dump.)

## 11. KEY REFRAME: the R's raw IS "Dpraw" = Dual Pixel RAW (DPAF). Cache + IRQ notes.
External review (Gemini) + our own sec-3 findings converge: the R raw pipeline is the **"Dpraw" = Dual
Pixel RAW** path (DprawHeadToRaw / DprawSap_Start / DprawCorrection). The R sensor is Dual Pixel CMOS AF --
every photosite has TWO photodiodes (A,B). So the raw buffer likely carries dual-pixel structure (A/B
interleaved per-column or as separate planes), NOT a plain single-pixel Bayer raster. **This probably
explains the streak/shear artifacts** in the wide dumps (idx2 etc.): a single-pixel width is half (or
double) the true layout. -> render with de-interleave: A=even cols, B=odd cols, and A+B sum (the normal
image is A+B). Added eosr_port/render_dpraw.py for this.

Cross-check vs the three classic DIGIC-8 raw traps (all already addressed by our approach):
1. CACHE COHERENCY -- HANDLED. R uncacheable alias = phys | 0x40000000 (mem_defs.h). We point EDMAC dest
   AND our reads at the uncached alias (UNCACHEABLE(cp)); no stale-cache reads.
2. EDMAC COMPLETION-IRQ HEARTBEAT -- this is why the earlier commandeer-a-channel slurp threw Err 70 +
   reboot: StartEDmac on a free channel WITHOUT RegisterEDmacComplete/Abort/PopCBR -> firmware saw a
   missing completion heartbeat -> Err 70 / state-machine crash. ***Our pivot SIDESTEPS this***: Raw-LV
   hook does NOT redirect/commandeer any EDMAC -- it enables Canon's own raw LV (lv_save_raw) and READS the
   buffer Canon fills, leaving Canon's IRQ/CBR machinery fully intact. (If we later need our own channel,
   we MUST register the completion CBRs, like mlv_lite edmac_raw_slurp does.)
3. CORRECT CHANNEL (FE raw before IPP, not display/EVF/DPAF-phase) -- exactly the current hunt: find the
   channel whose buffer is FE Bayer (pre-IPP) vs the debayered IPP previews. idx30 confirmed the scene;
   re-targeted dump {2,30,19,64,69,8} to find the Bayer (likely dual-pixel) one.

## 12. DECISIVE: the full-res RAW is NOT CPU-buffered in LV -> must SLURP conn0 (with CBRs)
Re-targeted dump {2,30,19,64,69,8} rendered. **idx30 @ width 640 = a CLEAN recognizable KITCHEN scene**
(wall oven, cabinets, backsplash, reflective counter) -- proves the whole read path works (hook -> a0 ->
UNCACHEABLE read -> render). BUT it's SMOOTH/debayered (a ~640-wide processed/preview plane), NOT Bayer.
Across all 11 dumped channels: idx30/69 = small debayered previews (scene, planar ~640, repeated); idx8/55/
45 = stats/structured (header row + black bands); idx2 = wide but streaks at EVERY interp incl dual-pixel
de-interleave (line-delay / DPAF-phase / non-raster); idx5/18 = sparse/noise. **NO full-res Bayer raster is
CPU-readable.**

=> This is EXACTLY why mlv_lite uses CONFIG_EDMAC_RAW_SLURP on newer DIGIC: the full-res sensor raw is NOT
parked in a readable buffer -- it streams through the EDMAC and must be SLURPED in real time from the sensor
connection (conn 0). The FUN_e05364b6 +0xa0 hook only ever sees post-processing buffers, so it cannot find
the raw. CONCLUSION: the readable-buffer approach is exhausted; the path to full-res raw is the EDMAC slurp.

***The slurp, done RIGHT this time*** (the earlier Err70/reboot was the missing completion-IRQ heartbeat,
per sec 11): RegisterEDmacCompleteCBR + AbortCBR + PopCBR on the chan BEFORE StartEDmac (mlv_lite
edmac_raw_slurp does this; I omitted it). The completion CBR ALSO gives per-frame timing -> we can re-arm
from the CBR instead of the EVF vsync hook (which crashes EvfCap on R). NEXT: RE the R's RegisterEDmacComplete
CBR / EDmac CBR table; build a CBR-driven slurp (conn0, dual-pixel geometry, CBRs registered); menu-invoked.

## 13. EDMAC completion-CBR API resolved (decompiled) + corrected "CBR slurp" built
Resolved the R's EDMAC CBR API (was SUSPECT in stubs.S):
- **RegisterEDmacCompleteCBR = `0xE0535A82`** -- `(chan, cbr, ctx)`: stores cbr@`DAT_e0535d98`+chan*8,
  ctx@+4, sets **pBlock+0x3c = 1** (enables the completion IRQ), calls FUN_e0554504. THIS is the heartbeat.
- **Unregister*CBR = `0xE0535AAE`** -- `(chan, mask)`: clears pBlock+0x34/+0x38/+0x3c by mask bits
  **0x20=Pop(+0x34), 0x10=Abort(+0x38), 0x08=Complete(+0x3c)**.
- CBR-enable flags live at **pBlock +0x34 (Pop) / +0x38 (Abort) / +0x3c (Complete)**.
- Confirmed `FUN_e0535a42` = combined start (calls FUN_e0535998 read-start + FUN_e053595e write-start), so
  **StartEDmac(write)=0xE053595E, StartEDmac(read)=0xE0535998** verified.
(Abort/Pop register fns not separately located yet -- mlv_lite's CBR is a no-op anyway; Complete is the one
that matters for the heartbeat. Add Abort/Pop only if Err70 persists.)

Built **"CBR slurp" (cbrslurp_task, md5 2f7d1826 @ 15:11):** movie LiveView (NO rec): RegisterEDmacComplete
CBR(idx7, cb, 0) -> ConnectWriteEDmac(idx7,0) -> FUN_e05364b6(idx7, ubuf) -> SetEDmac(idx7,0,0,geom) ->
StartEDmac(idx7); wait for the completion cb (per-frame timing, NOT EVF vsync) up to 2s; stop; unregister;
copy ubuf -> ML/LOGS/SLURP.BIN + SLURPS.TXT (logs cbr count + regs). Geometry 1920x1080 14-bit guess
(Dpraw may need 2x width). RISK: still commandeers idx7+conn0 -> if the CBR-heartbeat theory is wrong it may
Err70/reboot (recoverable, menu-invoked). NEXT: user runs it; SLURPS.TXT cbr>0 + a coherent Bayer SLURP.BIN
= RAW SLURPED (milestone). If Err70: add Abort/Pop CBRs / rethink. If blank/garbage: iterate geometry/conn.

## 14. CBR slurp crashed+rebooted (no Err70). IRQ table found; staged to pinpoint.
"CBR slurp" -> crash + reboot, NO Err70 (a kernel panic, not a recording error). Diagnosis:
- `FUN_e0554504` (called by RegisterEDmacCompleteCBR) = **RegisterInterruptHandler** (SystemIF::KerInt.c):
  asserts the IRQ id is in [1,0x1ff]; on bad id -> panic.
- **EDMAC completion IRQ-ID table @ `0xE0DD641C`** (ROM, {irq_id, handler} per chan, stride 8). EVERY
  channel has a VALID id incl **idx7 (irq=0xd1, handler=0xe05378d7** = Canon's generic EDMAC completion ISR,
  same for all chans). DAT refs: pBlock table DAT_e0535d30=0xE0DD5C64 (DmacInfo); cbr table DAT_e0535d98=
  0x00073de8 (RAM); irq table DAT_e0535d9c=0xE0DD641C (ROM).
- => the crash is NOT a missing IRQ id. It's deeper: re-RegisterInterruptHandler on an already-live IRQ
  (0xd1) and/or the StartEDmac transfer/completion on a commandeered free channel. The free-channel slurp
  keeps hitting firmware walls: bare -> Err70 at start; +CompleteCBR -> panic.
Built STAGED CBR slurp (md5 a04584bc @ 15:27): marker to SLURPS.TXT + NotifyBox before each op
(regcomplete/connw/setbuf/setedmac/start/waiting) -> the last marker = the crash step (survives reboot).
NEXT: user runs it, reports the last on-screen step (or SLURPS.TXT). That localizes the panic so we fix it
(skip the redundant re-register; or don't re-enable the IRQ; or the conn0 double-write is fundamental).

### Panic mechanism (likely regcomplete): SMP cross-core IRQ re-register
Decompiled the RegisterInterruptHandler path:
- `FUN_e0554504`(_,irq,handler,chan) -> `FUN_e011de06`(irq,handler,chan,0); panics (assert) only if the
  return is NEGATIVE.
- `FUN_e011de06` with param_4=0 calls `FUN_e011ddce`(irq, cur_cpu) and skips+returns its code on conflict.
- **`FUN_e011ddce` returns -1 (0xffffffff) iff the IRQ is already registered AND for a DIFFERENT CPU** than
  the caller's: `if (state!=0 && (1<<cur_cpu & state)==0) return -1;`.
The R is SMP (CONFIG_TASK_STRUCT_V2_SMP). If Canon registered idx7's EDMAC completion IRQ (0xd1) on core A
and our menu task runs on core B, RegisterEDmacCompleteCBR -> -1 -> **panic**. This fits the crash+reboot.
**FIX (if staged shows regcomplete): set the CBR DIRECTLY, skip RegisterInterruptHandler** -- the handler
0xe05378d7 is already wired (ROM table @0xE0DD641C); just do:
  *(uint32_t*)(0x00073de8 + chan*8)     = (uint32_t)slurp_complete_cbr;   // cbr table (DAT_e0535d98)
  *(uint32_t*)(0x00073de8 + chan*8 + 4) = 0;                              // ctx
  *(volatile uint32_t*)(*(uint32_t*)(0xE0DD5C64 + chan*8) + 0x3c) = 1;    // pBlock+0x3c = completion enable
i.e. replicate FUN_e0535a82's table writes WITHOUT its FUN_e0554504 call. (If staged shows start/waiting
instead, the panic is the transfer/ISR on a commandeered free chan -> pivot to a different chan/conn or
Canon's raw-channel infra, not this fix.)

## 15. PROVEN BLOCKER: commandeered-free-channel slurp is unworkable. Pivot to Canon's own raw channel.
Staged CBR slurp -> last marker "start" then HARD HANG (no reboot, battery pull; no Err70). So regcomplete/
connw/setbuf/setedmac all PASS; **StartEDmac is the wall**. Combined with the bare slurp (Err70 at start),
this proves: starting a commandeered FREE channel (idx7) on a sensor-raw connection is rejected -- bare ->
engine Err70; +completion CBR -> the completion IRQ fires into idx7's unprepared ISR path -> hard lock.
Free channels lack the transfer/ISR infra Canon sets up only for channels it actively uses. The SMP
re-register theory was wrong (regcomplete passed). ==> ABANDON the commandeer-a-free-channel slurp.

PIVOT (md5 fff5d8c5 @ 15:48): "Rec dump" (rec_dump_task) -- the proven +0xa0 hook + FULL 4MB buffer dump
of the candidates DURING H264 RECORDING (rawlv=0). The Dpraw (Dual Pixel RAW) record path (sec 3: record
evt -> DprawHeadToRaw -> FUN_e06d6bda programs the raw chan via FUN_e05364b6) should program Canon's own
full-res raw buffer, which our hook catches -- NO commandeering, NO StartEDmac, so it CANNOT hang. Also logs
the dpraw chan hint *(0x243ec)/*(0x24420). -> RAWHK.TXT + RW{2,30,19,64,69,8}.BIN. NEXT: user records ~12s;
render the dumps (render_full/render_dpraw) -> if Canon's recording raw is full-res Bayer, we read it
straight (then continuous capture from that channel). If still only previews: the full-res raw truly never
hits a CPU buffer (recording or LV) on this body -> document as the hard limit; the only remaining path is a
properly Canon-initialized channel/raw-LV-mode, which is a deep bring-up.

## 16. CONFIRMED HARD LIMIT: full-res raw is NOT CPU-readable in LV *or* recording
"Rec dump" during H264 recording: the hook caught essentially the SAME channels as plain LV (previews +
stats), with NEARLY IDENTICAL rowdiff per channel (idx30 19.6 vs 19.5, idx2 45.1 vs 44.4, ...). idx30@640
renders the SAME clean DEBAYERED kitchen. And **`dpraw: *243ec=00000000`** -> the DprawHeadToRaw config
pointer is NULL, so the Dual-Pixel-RAW record path is DORMANT in plain H264 (it needs DPRAW mode, a stills
feature). So the full-res Bayer raw does NOT land in any CPU-readable buffer, in LiveView OR during
recording -- only debayered previews + stats do.

### Where the EOS R raw bring-up stands (the map is complete; the goal is blocked)
ACHIEVED: validated runtime ROM-hook infra (MMU 2-page bump + function hooks); full EDMAC API
(SetEDmac/Connect/Start/CBR-register 0xE0535A82/IRQ-table 0xE0DD641C); Canon raw-LV enable (lv_set_mm+
lv_save_raw); read+render Canon's live image (the kitchen). BOUNDARIES PROVEN: (1) full-res raw not
CPU-buffered (LV+rec); (2) commandeer-free-channel slurp UNWORKABLE (StartEDmac -> Err70 bare / hard hang
+CBR -- free chans lack Canon's transfer/ISR infra); (3) EVF state hook crashes EvfCap.
REMAINING PATHS (all deep/uncertain): (A) TRIGGER the dormant Dpraw path (DprawSap_Start FUN_e071f574 /
make *243ec non-null) so Canon programs the full-res raw buffer we CAN read -- the most promising; needs
camera-free RE of the trigger first. (B) fully initialize a free channel the way Canon does (transfer/ISR/
Boomer/IRQ) -- very deep. (C) a true raw-LV bring-up. Recommendation: pursue (A) via camera-free RE before
any more camera tests.

## 17. Dpraw = Canon DUAL PIXEL RAW (a STILLS feature). Video raw = hard limit; stills raw IS reachable.
DprawSap_Start (FUN_e071f574) builds a "DprawSapCorrection" object + runs the HeadToRaw chain (4 ctx params
-> upstream-triggered). ROM strings settle what it is: **`CameraConductor::CC_PropLink_DPRAW.c`** (property-
linked), **`SCS_FaAllocateMemoryResourceForDpRawCaptureBuffer`**, **`FA_GetDPRawBuf`**, **`Set/GetDPRawImage
Buffer`**, **`Mem1ComponentForDPRaw`**, **`DPRAW_DARK`**. => Dpraw is Canon's **Dual Pixel RAW**, a *stills*
capture path (a photo, with a dedicated DpRaw capture buffer), NOT a video route. That's why *243ec was NULL
in plain H264 (the Dpraw config only allocates for a DPRAW photo capture).

CONCLUSION:
- **VIDEO full-res raw = HARD LIMIT** (confirmed not CPU-readable in LV/recording; slurp unworkable; EVF hook
  crashes; Dpraw is stills, not video). No accessible path on the R right now. Documented.
- **STILLS full-res raw = REACHABLE.** Enabling Canon's "Dual Pixel RAW" (a normal Photo menu setting) makes
  Canon allocate the DpRaw capture buffer + run DprawHeadToRaw, which programs the raw chan via FUN_e05364b6
  -- the EXACT leaf our +0xa0 hook taps. So the existing "Rec dump" (rawhk +0xa0 hook + full-buffer dump),
  run while taking a DPRAW PHOTO, should catch Canon's full-res raw buffer -> ML reads the full-res raw
  (the "ML control: read+save the raw" goal, for stills). No commandeering, no property write -> safe.
  This is the raw-track camera probe. (Each timelapse frame is already a CR3; this is ML reading the raw
  buffer directly, toward an ML DNG pipeline.)
=> Both tracks camera-ready: (A) adaptive timelapse test, (B) Dpraw-stills raw via "Rec dump" + a DPRAW photo.

## 18. COMPREHENSIVE RAW-PIXEL FLOW MAP (EOS R) -- when/where/how raw pixels move, + non-destructive taps
### Stages (sensor -> RAM)
1. Sensor -> Front-End (FE) / A-D -> 14-bit sensor raw (Dual Pixel: 2 photodiodes A/B per photosite).
2. FE raw -> EDMAC **connection 0** (the sensor-raw source; qemu-eos: conn 0/35 = sensor data).
3. From conn 0, the path FORKS by mode:
   - **LV / MOVIE:** the imaging engine (IPP) debayers conn0 upstream. What lands in CPU-readable RAM is
     only PROCESSED planes -- debayered previews (idx30 = the ~640-wide kitchen luma; idx4/5/6/...), stats,
     structured buffers. **The full-res 14-bit Bayer is NEVER written to a CPU-readable buffer in LV/movie**
     -> the video full-res raw is the HARD LIMIT (slurp hangs; EVF hook crashes; not buffered).
   - **STILLS (image quality RAW + Canon Dual Pixel RAW ON):** during a PHOTO, the still-capture pipeline
     programs the **0xD0487 EDMAC channels** (idx23-25, idx58-61) with the FULL-RES raw -- these channels are
     ABSENT in LV/movie. So the **stills full-res raw IS reachable** (the +0xa0 hook captured their buffer
     addrs). Observed (DPRAW photo): idx24 a0=a32df198, idx25 a0=a00038cc (hits 16/14); idx59 a0=76eea878,
     idx60 a0=6fa10000, idx61 a0=76f0df5c (hits 2).

### Memory / MMU (the read-path)
- R uncached aliasing (mem_defs.h, non-VXWORKS): **UNCACHEABLE(x)= x | (x<0x40000000 ? 0x40000000 : 0)**,
  CACHEABLE(x)= x & ~0x40000000. => **cached RAM 0x00000000-0x3FFFFFFF; uncached alias 0x40000000-0x7FFFFFFF**
  (1GB window). LV preview + stills buffers in 0x4x-0x7x are CPU-READABLE this way (proven: the kitchen render,
  idx59/60/61).
- **OPEN (MMU crux):** the active stills-raw buffers (idx24/25, a0=0xa0xxxxxx) are ABOVE the 0x40000000 alias
  window. CONFIG_MEM_2GB => 2GB RAM with an "unusual map"; 0xa0000000 is likely 2nd-GB RAM or a high uncached
  alias, but the alias bit (0x40000000? 0x80000000?) / validity is UNVERIFIED. Reading it blind from a task
  could data-abort. NEXT: determine 0xa0000000's mapping by RE'ing the still-capture allocator
  (SCS_FaAllocateMemoryResourceForDpRawCaptureBuffer @str e005836c / FA_GetDPRawBuf @str e0058df8) OR the
  Canon MMU TTBR/L1 table, to get the safe cached twin. THEN the dump can read idx24/25.

### Interception -- non-destructive (use) vs destructive (avoid)
- **NON-DESTRUCTIVE (validated):** the +0xa0 hook on FUN_e05364b6 reads each channel's buffer ADDRESS from
  Canon's own per-frame write -- it disturbs nothing (re-does the exact store), no DMA reconfiguration. Reading
  the buffer via the cached/uncached alias is a pure CPU read. This is THE safe interceptor.
- **DESTRUCTIVE (proven, avoid):** commandeering a free EDMAC channel + StartEDmac on conn0 -> Err70 / hard
  hang (free chans lack Canon's transfer/ISR infra). EVF state-object hook -> EvfCap crash. Reading the d0487
  channel REGISTERS directly -> data-abort (but the +0xa0 hook gives the buffer addr, sidestepping this).
