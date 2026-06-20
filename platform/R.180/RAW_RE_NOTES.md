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
