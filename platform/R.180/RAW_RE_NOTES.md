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

Open variables: a **free write channel** with a BoomerID (one NOT in the `FUN_e05364b6` hook log:
idx0,1,3,7,9,10,12,13,15,16,20,22,25,32-38 — experiment), the actual **width/height**, and whether a
parallel connection to conn 0 disrupts Canon's recording. Iterate by camera test until a coherent Bayer
frame lands (pixel-level Bayer test: adjacent-pixel diff >> alternate-pixel diff).
