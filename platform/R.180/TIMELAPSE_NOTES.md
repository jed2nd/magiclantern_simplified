# EOS R — Adaptive-exposure timelapse (holy-grail day->night) — build notes

Goal: an intervalometer that RAMPS exposure between shots so a day->night sequence stays evenly
exposed (no flicker). Each frame is a normal photo -> a full CR3 RAW = the "trigger+capture+record
RAW stills" goal. The R has no raw-LV histogram (ML Auto ETTR can't run), so we meter the LiveView
luma instead.

## Pieces (all validated on this branch)
- METER: `get_lv_avg_luma()` (shoot.c) -- average Y of the LiveView UYVY image, 0-255 = scene brightness.
- ACTUATORS: `lens_set_rawshutter` (R hi-byte), `lens_set_rawiso` (R byte-1), `lens_set_rawaperture`
  -- all camera-validated read/write (PATH1_CAPTURE_STATE.md / PR #280).
- CADENCE: the intervalometer (works on R).

## Algorithm (`adaptive_exposure_step()`, shoot.c, FEATURE_INTERVALOMETER)
Meter avgY; err = avgY - target(92); if |err|>deadband(8): raw_step = clamp(err/12, +-maxstep(3 ML
raw = ~1/3 stop)) -- anti-flicker. Apply to shutter within [shut.min..max]; the part the shutter range
can't absorb spills to ISO within [iso.min..max] (bright->faster shutter/lower ISO; dark->slower/higher).
Tracks adapt_cur_shutter/iso internally (R shutter dial read is unreliable; we own the value).

## Build progress
- [x] STEP 1+2: get_lv_avg_luma() + adaptive_exposure_step() + CONFIG_INT tunables. Compiles, qemu OK.
- [x] STEP 3: "Adaptive exposure" controls added as Intervalometer submenu children (on/off, target, max step, shutter floor/ceiling, ISO ceiling). Compiles, qemu OK. _bss_end=0x144100 (watch budget).
- [x] STEP 4: adaptive_exposure_step() wired into the intervalometer loop (shoot.c ~6248, start of each interval when adapt_exp_enabled && lv -- meters before idle-powersave, fresh each frame). Compiles, qemu OK, _bss_end=0x144300.
- [>] READY TO TEST. camera test: intervalometer + adaptive ON, dim a light, confirm frames stay even.
Tunables (defaults): target 92, maxstep 3, shut [40..152], iso [72..112] (ISO100..6400). Tune on camera.
