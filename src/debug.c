/** \file
 * Magic Lantern debugging and reverse engineering code
 */
#include "dryos.h"
#include "bmp.h"
#include "tasks.h"
#include "debug.h"
#include "menu.h"
#include "property.h"
#include "config.h"
#include "gui.h"
#include "lens.h"
#include "version.h"
#include "edmac.h"
#include "patch.h"
#include "asm.h"
#include "beep.h"
#include "screenshot.h"
#include "console.h"
#include "zebra.h"
#include "shoot.h"
#include "cropmarks.h"
#include "fw-signature.h"
#include "lvinfo.h"
#include "raw.h"
#include "rom_values.h"

#ifdef CONFIG_DEBUG_INTERCEPT
#include "dm-spy.h"
#include "tp-spy.h"
#endif

#ifdef CONFIG_MODULES
#include "module.h"
#endif

#if defined(CONFIG_600D) && defined(CONFIG_AUDIO_600D_DEBUG)
void audio_reg_dump_once();
#endif

#if defined(CONFIG_EDMAC_MEMCPY)
#include "edmac-memcpy.h"
#endif

extern int config_autosave;
extern void config_autosave_toggle(void* unused, int delta);

static struct semaphore * beep_sem = 0;

static void debug_init_func()
{
    beep_sem = create_named_semaphore("beep_sem", SEM_CREATE_UNLOCKED);
}
INIT_FUNC("debug", debug_init_func);

void NormalDisplay();
void MirrorDisplay();
static void HijackFormatDialogBox_main();
void debug_menu_init();
void display_on();
void display_off();


void fake_halfshutter_step();

#ifdef CONFIG_DEBUG_INTERCEPT
void j_debug_intercept() { debug_intercept(); }
void j_tp_intercept() { tp_intercept(); }
#endif

#if CONFIG_DEBUGMSG
static int draw_prop = 0;

static int dbg_propn = 0;
static void
draw_prop_reset( void * priv )
{
    dbg_propn = 0;
}
#endif

void _card_led_on()
{
#ifdef CARD_LED_ADDRESS
    *(volatile uint32_t*) (CARD_LED_ADDRESS) = (LEDON);
#endif
}

void _card_led_off()
{
#ifdef CARD_LED_ADDRESS
    *(volatile uint32_t*) (CARD_LED_ADDRESS) = (LEDOFF);
#endif
}

void info_led_on()
{
    #ifdef CONFIG_VXWORKS
    LEDBLUE = LEDON;
    #elif defined(CONFIG_BLUE_LED)
    call("EdLedOn");
    #else
    _card_led_on();
    #endif
}
void info_led_off()
{
    #ifdef CONFIG_VXWORKS
    LEDBLUE = LEDOFF;
    #elif defined(CONFIG_BLUE_LED)
    call("EdLedOff");
    #else
    _card_led_off();
    #endif
}
void info_led_blink(int times, int delay_on, int delay_off)
{
    for (int i = 0; i < times; i++)
    {
        info_led_on();
        msleep(delay_on);
        info_led_off();
        msleep(delay_off);
    }
}

static void dump_rom_task(void* priv, int unused)
{
    msleep(200);
    FILE * f = NULL;

// Digic 6 doesn't have ROM0
#if defined(ROM0_SIZE) && (ROM0_SIZE != 0)
    // this skips D6 by default, which we've never seen with ROM0
    f = FIO_CreateFile("ML/LOGS/ROM0.BIN");
    if (f)
    {
        bmp_printf(FONT_LARGE, 0, 60, "Writing ROM0");
        FIO_WriteFile(f, (void*)ROM0_ADDR, ROM0_SIZE);
        FIO_CloseFile(f);
    }
    msleep(200);
#endif

#if defined(ROM1_SIZE) && (ROM1_SIZE != 0)
    f = FIO_CreateFile("ML/LOGS/ROM1.BIN");
    if (f)
    {
        bmp_printf(FONT_LARGE, 0, 60, "Writing ROM1");
        FIO_WriteFile(f, (void*)ROM1_ADDR, ROM1_SIZE);
        FIO_CloseFile(f);
    }
    msleep(200);
#endif

    dump_big_seg(4, "ML/LOGS/RAM4.BIN");
}

static void dump_img_task(void* priv, int unused)
{
    for (int i = 5; i > 0; i--)
    {
        NotifyBox(1000, "Will dump VRAMs in %d s...", i);
        msleep(1000);
    }
    NotifyBox(5000, "Dumping VRAMs...");
    
    FILE * f = NULL;
    char pattern[0x80];
    char filename[0x80];
    
    char* video_mode = get_video_mode_name(0);
    char* display_device = get_display_device_name();
    

    int path_len = snprintf(pattern, sizeof(pattern), "%s/%s/%s/", CAMERA_MODEL, video_mode, display_device);
    
    /* make sure the VRAM parameters are updated */
    get_yuv422_vram();
    get_yuv422_hd_vram();

    snprintf(pattern + path_len, sizeof(pattern) - path_len, "LV-%%03d.422", 0);
    get_numbered_file_name(pattern, 999, filename, sizeof(filename));
    f = FIO_CreateFile(filename);
    if (f)
    {
        FIO_WriteFile(f, vram_lv.vram, vram_lv.height * vram_lv.pitch);
        FIO_CloseFile(f);
    }

    snprintf(pattern + path_len, sizeof(pattern) - path_len, "HD-%%03d.422", 0);
    get_numbered_file_name(pattern, 999, filename, sizeof(filename));
    f = FIO_CreateFile(filename);
    if (f)
    {
        FIO_WriteFile(f, vram_hd.vram, vram_hd.height * vram_hd.pitch);
        FIO_CloseFile(f);
    }

#ifdef CONFIG_RAW_LIVEVIEW
    snprintf(pattern + path_len, sizeof(pattern) - path_len, "RAW-%%03d.DNG", 0);
    get_numbered_file_name(pattern, 999, filename, sizeof(filename));
    
    if (lv) raw_lv_request();
    if (raw_update_params())
    {
        /* first frames right after enabling the raw buffer might be corrupted, figure out why */
        /* todo: fix it in the raw backend */
        wait_lv_frames(3);
        raw_set_dirty();
        raw_update_params();
        
        /* make a copy of the raw buffer, because it's being updated while we are saving it */
        void* buf = malloc(raw_info.frame_size);
        if (buf)
        {
            memcpy(buf, raw_info.buffer, raw_info.frame_size);
            struct raw_info local_raw_info = raw_info;
            local_raw_info.buffer = buf;
            save_dng(filename, &local_raw_info);
            free(buf);
        }
    }
    if (lv) raw_lv_release();
    
    if (!is_file(filename))
    {
        /* if we don't have any raw data, create an empty DNG just to keep file numbering consistent */
        f = FIO_CreateFile(filename);
        FIO_CloseFile(f);
    }
#endif

    /* create a log file with relevant settings */
    snprintf(pattern + path_len, sizeof(pattern) - path_len, "VRAM-%%03d.LOG", 0);
    get_numbered_file_name(pattern, 999, filename, sizeof(filename));
    f = FIO_CreateFile(filename);
    if (f)
    {
        my_fprintf(f, "display=%d (hdmi=%d code=%d rca=%d)\n", EXT_MONITOR_CONNECTED, ext_monitor_hdmi, hdmi_code, _ext_monitor_rca);
        my_fprintf(f, "lv=%d (zoom=%d dispmode=%d rec=%d)\n", lv, lv_dispsize, lv_disp_mode, RECORDING_H264);
        my_fprintf(f, "movie=%d (res=%d crop=%d fps=%d)\n", is_movie_mode(), video_mode_resolution, video_mode_crop, video_mode_fps);
        my_fprintf(f, "play=%d (ph=%d, mv=%d, qr=%d)\n", PLAY_MODE, is_pure_play_photo_mode(), is_pure_play_movie_mode(), QR_MODE);
        
        FIO_CloseFile(f);
    }

    NotifyBox(2000, "Done :)");
    beep();
}

#ifdef FEATURE_GUIMODE_TEST
// beware, might be dangerous, some gui modes will give errors
void guimode_test()
{
    msleep(1000);
    for (int i = 0; i < 99; i++)
    {
        // some GUI modes may lock-up the camera or reboot
        // if this is the case, the troublesome mode will be skipped at next reboot.
        char fn[50];
        snprintf(fn, sizeof(fn), "VRAM%d.BMP", i);

        if (FIO_GetFileSize_direct(fn) != 0xFFFFFFFF) // this gui mode was already tested?
            continue;

        NotifyBox(500, "Trying GUI mode %d...", i);
        save_mem_to_file(fn, 0, fn); // temporary flag to indicate that this GUI mode was tried (and probably found to be troublesome)
        msleep(200);

        SetGUIRequestMode(i);

        msleep(1000);
        FIO_RemoveFile(fn);

        take_screenshot(SCREENSHOT_FILENAME_AUTO, SCREENSHOT_BMP);

        // try to reset to initial gui mode
        SetGUIRequestMode(0);
        SetGUIRequestMode(1);
        SetGUIRequestMode(0);

        msleep(1000);
    }
}
#endif

static void run_test()
{
    DryosDebugMsg(0, 15, "run_test fired");

    // This is useful for devs, via normal users, as it dumps the internal
    // log to disk as log0000.log.  This holds DryosDebugMsg() messages,
    // before sending to uart.  Holds more events than you see on uart,
    // since dm_store is more permissive than dm_print.
    call("dumpf");

#ifdef CONFIG_R
    /* dump captured MPU boot-spell log to ML/LOGS/MPULOG.TXT
     * (capture task started in boot_pre_init_task) */
    extern void mpu_capture_dump(void);
    mpu_capture_dump();
#endif

#if 0 && defined(CONFIG_200D)
    // Want to run a quick test?  You can hack it in here,
    // after modifying the above guards.  The guards allow
    // you to hack in whatever hard-coded per cam constants
    // you want, if you're doing that kind of thing.
#endif

}

#ifdef CONFIG_R
/* R image-capture probe (Debug -> "Take test pic (no AF)").
 * Calls take_a_pic(AF_DONT_CHANGE) -- AF_DONT_CHANGE skips lens_setup_af, the
 * suspected null-pointer crash path. Brackets the call with SHOOT0/SHOOT9 marker
 * files; lens_take_picture writes the detailed step trail to ML/LOGS/SHOOT.TXT.
 * After: SHOOT9.TXT present = completed; absent = crashed (see SHOOT.TXT). */
static void shoot_test_task()
{
    extern int take_a_pic(int should_af);
    gui_stop_menu();
    msleep(500);

    FILE * f = FIO_CreateFile("ML/LOGS/SHOOT0.TXT");
    if (f) { FIO_WriteFile(f, (void *)"0: shoot_test_task start\n", 25); FIO_CloseFile(f); }
    msleep(50);

    take_a_pic(AF_DONT_CHANGE);

    f = FIO_CreateFile("ML/LOGS/SHOOT9.TXT");
    if (f) { FIO_WriteFile(f, (void *)"9: take_a_pic returned\n", 23); FIO_CloseFile(f); }
}

/* R AF-capture probe (Debug -> "Take test pic (SW1/SW2 AF)").
 * Drives a real half-press (SW1 -> meter + autofocus) then full-press
 * (SW2 -> capture), which call("Release") alone does not do on the R.
 * Breadcrumbs -> ML/LOGS/SHOOTAF.TXT. Needs PROP_REMOTE_SW1/SW2 whitelisted. */
static char afbc[256];
static int  afbc_len;
static void af_bc(const char * s)
{
    if (afbc_len > (int)sizeof(afbc) - 48) return;
    afbc_len += snprintf(afbc + afbc_len, sizeof(afbc) - afbc_len, "%s\n", s);
    FILE * f = FIO_CreateFile("ML/LOGS/SHOOTAF.TXT");
    if (f) { FIO_WriteFile(f, afbc, afbc_len); FIO_CloseFile(f); }
    msleep(30);
}
static void shoot_test_af_task()
{
    extern void fake_simple_button(int bgmt_code);
    extern int  get_focus_confirmation(void);
    char b[80];
    gui_stop_menu();
    msleep(800);
    afbc_len = 0;

    /* inject the actual half-shutter button event (triggers metering + AF),
     * the way the physical shutter does -- PROP_REMOTE_SW1 had no effect */
    af_bc("1: fake half-shutter (AF)");
    fake_simple_button(BGMT_PRESS_HALFSHUTTER);   /* 0x7D */
    msleep(2000);                                 /* let AF run/lock */
    snprintf(b, sizeof(b), "2: focusconf=%d", get_focus_confirmation());
    af_bc(b);

    /* capture (call("Release") is proven to capture on the R) */
    af_bc("3: call(Release) -> capture");
    call("Release");
    msleep(1200);

    /* release the half-shutter -- otherwise Canon stays in the shooting/
     * metering state and won't finish ("saving..." hang at power-off) */
    af_bc("4: release half-shutter");
    fake_simple_button(BGMT_PRESS_HALFSHUTTER + 1);   /* 0x7E = unpress half-shutter */
    msleep(300);
    af_bc("5: done");
}

static void mpu_capture_arm_menu()
{
    extern void mpu_capture_arm(void);
    gui_stop_menu();
    mpu_capture_arm();
    NotifyBox(2000, "MPU capture armed");
}

/* flush + write the passive serial-flash read capture (defined in init.c,
 * installed in boot_pre_init_task). Writes ML/LOGS/TUNE.BIN + SFREAD.TXT. */
static void sfread_capture_dump_menu()
{
    extern void sfread_capture_dump(void);
    gui_stop_menu();
    msleep(300);
    sfread_capture_dump();
}

/* ACTIVE serial-flash read (defined in init.c). Calls the real RBSF on cpu0 ourselves rather than
 * waiting to observe Canon's boot-time reads (which finish before ML loads). -> TUNE.BIN+SFACTIVE.TXT. */
static void sfread_active_read_menu()
{
    extern void sfread_active_read(void);
    gui_stop_menu();
    msleep(300);
    sfread_active_read();
}

/* SRM probe (Debug -> "Test SRM alloc"). SRM is disabled on the R
 * (CONFIG_MEMORY_SRM_NOT_WORKING: SRM_AllocateMemoryResourceFor1stJob crashes).
 * Call it directly (RscMgr FUN_e04e41be @0xE04E41BE) with a logging callback +
 * breadcrumbs to learn whether it works or exactly where it dies.
 * Result files: SRM0 (entered) / SRMCBR (callback fired = WORKS, with buf+size)
 * / SRM9 (call returned). If SRM0 only -> crashed inside the SRM call. */
/* v2: set globals only (FIO is unreliable in the resource-manager callback
 * context); the task reports them afterward. */
static volatile int      srm_cbr_fired = 0;
static volatile uint32_t srm_cbr_buf = 0, srm_cbr_size = 0;
static void srm_test_cbr(void ** dst_ptr, void * raw_buffer, uint32_t raw_size)
{
    srm_cbr_fired = 1;
    srm_cbr_buf  = (uint32_t) raw_buffer;
    srm_cbr_size = raw_size;
    if (dst_ptr) *dst_ptr = raw_buffer;
}
/* EEPROM dump (Debug -> "Dump EEPROM"). Uses the firmware's own working
 * ReadBlockEEPROM (FUN_e03d404e @0xE03D404E) -- the EEPROM is a small SPI
 * device on SIO3 (0xC0820000), a different channel than the serial flash, so
 * this read does NOT deadlock the way the sf_dump serial-flash read did.
 * readEEP(addr, dest, size) returns 0 on success. Dumps up to 32KB (the size
 * InstEEP implies, piVar1[3]=0x8000) in 0x100 chunks -> ML/LOGS/EEPROM.BIN.
 * Gives qemu the EEPROM/[EEP] config data it currently reads as zeros. */
static void eeprom_dump_task()
{
    int (*readEEP)(uint32_t, void *, uint32_t) = (void *)0xE03D404Fu;  /* thumb */
    static uint8_t eepbuf[0x100];
    int total = 0, lastret = 0;
    gui_stop_menu();
    msleep(500);
    FILE * f = FIO_CreateFile("ML/LOGS/EEPROM.BIN");
    for (uint32_t a = 0; a < 0x8000; a += 0x100) {
        lastret = readEEP(a, eepbuf, sizeof(eepbuf));
        if (lastret != 0) break;
        if (f) FIO_WriteFile(f, eepbuf, sizeof(eepbuf));
        total += sizeof(eepbuf);
    }
    if (f) FIO_CloseFile(f);
    FILE * g = FIO_CreateFile("ML/LOGS/EEPSTEP.TXT");
    if (g) { char b[64]; int n = snprintf(b, sizeof(b), "EEPROM dumped %d bytes, lastret=%d\n", total, lastret); FIO_WriteFile(g, b, n); FIO_CloseFile(g); }
}

/* Clean-capture probe (Debug -> "Take pic (IR remote)"). ML's call("Release")
 * capture leaves the shooting job unfinalized -> "saving" hang at power-off;
 * the PHYSICAL shutter is clean. SetEventIrRemoteReleaseBtn (FUN_e0190214)
 * drives the same CameraConductor remote-release pipeline a wireless remote
 * uses (1=press, 0=release) -> a full, properly-finalized capture. Test whether
 * this avoids the saving hang. Breadcrumbs -> ML/LOGS/IRREL.TXT. */
static void ir_release_task()
{
    void (*ir_rel)(int) = (void *)0xE0190215u;  /* thumb: SetEventIrRemoteReleaseBtn */
    FILE * f;
    gui_stop_menu();
    msleep(500);
    f = FIO_CreateFile("ML/LOGS/IRREL.TXT");
    if (f) { FIO_WriteFile(f, (void *)"1: IR remote press\n", 19); FIO_CloseFile(f); }
    ir_rel(1);                 /* remote button press (SW2-equivalent) */
    msleep(300);
    ir_rel(0);                 /* remote button release */
    msleep(2000);
    f = FIO_CreateFile("ML/LOGS/IRREL2.TXT");
    if (f) { FIO_WriteFile(f, (void *)"2: IR remote released, capture issued\n", 38); FIO_CloseFile(f); }
}

/* Exposure-write probe (Debug -> "Test shutter write"). ML's prop_set_rawshutter
 * writes PROP_SHUTTER with hardcoded len=4, but the R delivers it as 2 bytes, so
 * prop_request_change's length check blocks the write (the red box seen during
 * bracketing) and exposure can't be varied. This tests a 2-byte write directly:
 * nudge shutter ~1 stop, read back, restore. M mode + fully reversible.
 * -> ML/LOGS/SHUTWR.TXT */
static void shutter_write_test_task()
{
    char b[400]; int n = 0;
    gui_stop_menu();
    msleep(600);
    int before = lens_info.raw_shutter;
    n += snprintf(b + n, sizeof(b) - n, "before: raw_shutter=%d raw_iso=%d\n",
                  before, lens_info.raw_iso);
    int target = (before >= 24 && before <= 144) ? before + 8 : 96; /* ~1 stop, safe range */
    n += snprintf(b + n, sizeof(b) - n, "writing PROP_SHUTTER=%d with len=2 ...\n", target);
    prop_request_change(PROP_SHUTTER, &target, 2);   /* 2-byte write (R's actual length) */
    msleep(600);
    int after = lens_info.raw_shutter;
    n += snprintf(b + n, sizeof(b) - n, "after:  raw_shutter=%d -> %s\n",
                  after, (after == target) ? "CHANGED (2-byte write WORKS)" : "unchanged");
    prop_request_change(PROP_SHUTTER, &before, 2);   /* restore */
    msleep(400);
    n += snprintf(b + n, sizeof(b) - n, "restored: raw_shutter=%d\n", lens_info.raw_shutter);
    FILE * f = FIO_CreateFile("ML/LOGS/SHUTWR.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
}

/* PATH 1 capture-EXPOSURE matrix (Debug -> "Capture exposure test").
 * BRKSHUT.TXT proved ML sets the per-frame shutter correctly (raw 68..132) but every
 * IR-remote-release frame came out ~1" -- i.e. the IR-remote release captures at AE/
 * metered exposure and IGNORES the manual shutter. This probe fires ONE shot per capture
 * METHOD, each at a fixed VERY FAST shutter (raw 140 ~ 1/4000). A method that RESPECTS the
 * manual shutter -> a near-BLACK frame; an AE method -> a normally-exposed frame. Whichever
 * method comes out dark is the one to drive bracketing with.
 * Use M mode, ~1/125 dial, normal room, lens able to fire. -> ML/LOGS/CAPEXP.TXT.
 * Report which shot numbers (1-4) are DARK vs NORMAL. */
static char capx[480]; static int capx_n;
static void capx_bc(const char * s)
{
    if (capx_n > (int)sizeof(capx) - 72) return;
    capx_n += snprintf(capx + capx_n, sizeof(capx) - capx_n, "%s\n", s);
    FILE * f = FIO_CreateFile("ML/LOGS/CAPEXP.TXT");
    if (f) { FIO_WriteFile(f, capx, capx_n); FIO_CloseFile(f); }
    msleep(20);
}
static void capx_set_fast(void)
{
    int sh = 140;   /* raw ~1/4000: several stops under a normal/indoor exposure */
    prop_request_change(PROP_SHUTTER, &sh, 2);
    msleep(250);
    char b[72]; snprintf(b, sizeof(b), "   set raw=140 -> now=%d", lens_info.raw_shutter);
    capx_bc(b);
}
static void cap_expo_matrix_task(void)
{
    extern void fake_simple_button(int bgmt_code);
    void (*ir_rel)(int) = (void *)0xE0190215u;  /* SetEventIrRemoteReleaseBtn */
    int before;
    gui_stop_menu();
    msleep(800);
    capx_n = 0;
    before = lens_info.raw_shutter;
    capx_bc("CAPEXP: each shot raw=140 (~1/4000). DARK=manual respected, NORMAL=AE/metered.");

    /* Shot 1: IR-remote release alone (baseline -- expect NORMAL, confirms the AE finding) */
    capx_bc("shot 1: IR-remote release alone");
    capx_set_fast();
    ir_rel(1); msleep(300); ir_rel(0);
    msleep(3500);

    /* Shot 2: call("Release") alone (different capture path -- may respect manual) */
    capx_bc("shot 2: call(Release) alone");
    capx_set_fast();
    call("Release");
    msleep(3500);
    ir_rel(0);                 /* nudge the job to finalize */
    msleep(1000);

    /* Shot 3: half-press SW1 (locks metering, as a physical press does) THEN IR-remote */
    capx_bc("shot 3: SW1 half-press (held) + IR-remote release");
    capx_set_fast();
    fake_simple_button(BGMT_PRESS_HALFSHUTTER);       /* 0x7D press */
    msleep(1200);
    ir_rel(1); msleep(300); ir_rel(0);
    msleep(2800);
    fake_simple_button(BGMT_PRESS_HALFSHUTTER + 1);   /* 0x7E release */
    msleep(900);

    /* Shot 4: half-press SW1 (held) THEN call("Release") */
    capx_bc("shot 4: SW1 half-press (held) + call(Release)");
    capx_set_fast();
    fake_simple_button(BGMT_PRESS_HALFSHUTTER);
    msleep(1200);
    call("Release");
    msleep(2800);
    fake_simple_button(BGMT_PRESS_HALFSHUTTER + 1);
    msleep(900);

    /* restore the original shutter */
    prop_request_change(PROP_SHUTTER, &before, 2);
    msleep(300);
    capx_bc("done. Which shots (1-4) are DARK (manual) vs NORMAL (AE)?");
}

static void srm_test_task()
{
    void (*srm_alloc)(void *, void *) = (void *)0xE04E41BFu;  /* thumb */
    void * dst = 0;
    FILE * f;
    gui_stop_menu();
    msleep(500);
    srm_cbr_fired = 0; srm_cbr_buf = 0; srm_cbr_size = 0;
    f = FIO_CreateFile("ML/LOGS/SRM0.TXT");
    if (f) { FIO_WriteFile(f, (void *)"1: about to call SRM alloc @E04E41BE\n", 37); FIO_CloseFile(f); }
    msleep(50);

    srm_alloc((void *)srm_test_cbr, &dst);   /* async: callback fires when buffer ready */

    msleep(6000);                            /* wait longer for the async resource callback */
    f = FIO_CreateFile("ML/LOGS/SRMRES.TXT");
    if (f) {
        char b[128];
        int n = snprintf(b, sizeof(b), "fired=%d buf=0x%x size=0x%x (%d MB) dst=0x%x\n",
                         srm_cbr_fired, (unsigned)srm_cbr_buf, (unsigned)srm_cbr_size,
                         (int)(srm_cbr_size >> 20), (unsigned)dst);
        FIO_WriteFile(f, b, n); FIO_CloseFile(f);
    }
}

/* Dump the EEPROM driver struct (base 0x4CD4) to find the EEPROM's SIO channel
 * + CS register for the qemu EEPROM emulation. */
static void eeprom_struct_dump_task()
{
    char ib[1200]; int n = 0;
    gui_stop_menu(); msleep(300);
    n += snprintf(ib + n, sizeof(ib) - n, "EEPROM struct @0x4CD4:\n");
    for (uint32_t a = 0x4CD4; a < 0x4DD4; a += 16) {
        n += snprintf(ib + n, sizeof(ib) - n, "%08X:", a);
        for (int i = 0; i < 16; i += 4)
            n += snprintf(ib + n, sizeof(ib) - n, " %08X", MEM(a + i));
        n += snprintf(ib + n, sizeof(ib) - n, "\n");
    }
    FILE * f = FIO_CreateFile("ML/LOGS/EEPSTRUCT.TXT");
    if (f) { FIO_WriteFile(f, ib, n); FIO_CloseFile(f); }
}

/* PATH 2 (qemu): dump the camera's loaded FROM/property package DB so we can
 * feed it to the emulator, whose property loaders hang for lack of this data
 * (the R keeps it in SPI serial flash, not memory-mapped -> ROM1 is blank).
 * SearchFromProperty walks a list whose head pointer lives at 0xE054D758.
 * Walk the list and dump each node's words; pointer-looking words (RAM range)
 * get followed one level so the property data lands in the same dump. */
static void from_dump_task()
{
    static char buf[0x8000];
    int n = 0;
    gui_stop_menu(); msleep(500);
    uint32_t head = MEM(0xE054D758);
    n += snprintf(buf + n, sizeof(buf) - n, "DAT_e054d758 -> head=0x%08X\n", head);
    uint32_t node = head;
    int guard = 0;
    while (guard++ < 40 && node >= 0x1000 && node < 0x40000000) {
        n += snprintf(buf + n, sizeof(buf) - n, "n%02d %08X:", guard, node);
        for (int i = 0; i < 24; i++)
            n += snprintf(buf + n, sizeof(buf) - n, " %08X", MEM(node + i * 4));
        n += snprintf(buf + n, sizeof(buf) - n, "\n");
        /* follow pointer-looking words one level (capture the actual data) */
        for (int i = 0; i < 24 && n < (int)sizeof(buf) - 300; i++) {
            uint32_t p = MEM(node + i * 4);
            if (p >= 0x1000 && p < 0x40000000 && (p & 3) == 0) {
                n += snprintf(buf + n, sizeof(buf) - n, "  +%02X->%08X:", i * 4, p);
                for (int j = 0; j < 8; j++)
                    n += snprintf(buf + n, sizeof(buf) - n, " %08X", MEM(p + j * 4));
                n += snprintf(buf + n, sizeof(buf) - n, "\n");
            }
        }
        uint32_t next = MEM(node + 4);
        if (next == head || next == node) break;
        node = next;
        if (n > (int)sizeof(buf) - 1200) break;
    }
    FILE * f = FIO_CreateFile("ML/LOGS/PKGDUMP.TXT");
    if (f) { FIO_WriteFile(f, buf, n); FIO_CloseFile(f); }
    NotifyBox(4000, "PKGDUMP: %d nodes, %d B", guard - 1, n);
}

/* PATH 2 step 2: dump the exact FROM source regions the property handles point
 * at (from PKGDUMP analysis). The E1xxxxxx regions live in ROM0 (qemu already
 * has them); the F0xxxxxx regions are serial-flash-backed and blank in qemu --
 * those are what the loaders hang without. Check readability first (FROMCHK),
 * then dump any region that holds real data -> SFDATA.BIN (concatenated,
 * order = the F0 regions below). */
static void from_region_dump_task()
{
    static const uint32_t regions[] = { 0xF09C0000, 0xF0A80000, 0xF0AC0000, 0xF0B00000,
                                        0xE1FFC000, 0xE1C60000 };
    static const uint32_t sizes[]   = { 0x40000, 0x40000, 0x40000, 0x40000, 0x1000, 0x10000 };
    static uint8_t buf[0x4000];  /* chunk buffer (kept small to limit BSS) */
    char cb[600]; int cn = 0;
    gui_stop_menu(); msleep(400);

    cn += snprintf(cb + cn, sizeof(cb) - cn, "FROM region readability (first 4 words):\n");
    for (int i = 0; i < 6; i++)
        cn += snprintf(cb + cn, sizeof(cb) - cn, "%08X: %08X %08X %08X %08X\n",
                       regions[i], MEM(regions[i]), MEM(regions[i] + 4),
                       MEM(regions[i] + 8), MEM(regions[i] + 12));

    /* dump the 4 serial-flash-backed regions (skip blank ones) */
    FILE * f = FIO_CreateFile("ML/LOGS/SFDATA.BIN");
    for (int i = 0; i < 4 && f; i++) {
        int blank = (MEM(regions[i]) == 0xFFFFFFFF && MEM(regions[i] + 4) == 0xFFFFFFFF);
        cn += snprintf(cb + cn, sizeof(cb) - cn, "%08X: %s\n", regions[i],
                       blank ? "BLANK (needs SF driver)" : "DATA -> dumping");
        if (blank) continue;
        for (uint32_t off = 0; off < sizes[i]; off += sizeof(buf)) {
            for (uint32_t j = 0; j < sizeof(buf); j += 4)
                *(uint32_t *)(buf + j) = MEM(regions[i] + off + j);
            FIO_WriteFile(f, buf, sizeof(buf));
        }
    }
    if (f) FIO_CloseFile(f);

    FILE * c = FIO_CreateFile("ML/LOGS/FROMCHK.TXT");
    if (c) { FIO_WriteFile(c, cb, cn); FIO_CloseFile(c); }
    NotifyBox(4000, "FROM regions checked + dumped");
}

/* PATH 1: capture-done signal probe. The bracket drops frames because the R has
 * no reliable "capture complete / ready" signal (job_state delivers inconsistently;
 * the file-number wait is a model-dependent fallback). Fire 3 captures via the same
 * IR-remote path the bracket uses, and log how the 3 candidate signals evolve after
 * each release -> ML/LOGS/CAPSIG.TXT. Reveals which signal reliably tracks a capture
 * (and whether each release even produces one). */
static void capsig_probe_task()
{
    static char b[3800];
    int n = 0;
    gui_stop_menu(); msleep(800);
    n += snprintf(b + n, sizeof(b) - n, "Capture-signal probe (R) -- 3 shots via IR-remote\n");
    void (*ir_remote_release)(int) = (void *)0xE0190215u; /* SetEventIrRemoteReleaseBtn */
    for (int shot = 0; shot < 3; shot++)
    {
        int fnb = get_shooting_card()->file_number;
        n += snprintf(b + n, sizeof(b) - n, "--- shot %d (pre: job_state=0x%x burst=%d file=%d) ---\n",
                      shot + 1, lens_info.job_state, burst_count, fnb);
        ir_remote_release(1); msleep(300); ir_remote_release(0);
        int t0 = get_ms_clock();
        int ljs = -1, lbc = -1, lfn = -1;
        for (int i = 0; i < 130 && n < (int)sizeof(b) - 90; i++)   /* ~2.6s */
        {
            int js = lens_info.job_state, bc = burst_count, fn = get_shooting_card()->file_number;
            if (js != ljs || bc != lbc || fn != lfn)
            {
                n += snprintf(b + n, sizeof(b) - n, "  t=%5d ms: job_state=0x%x burst=%d file=%d\n",
                              get_ms_clock() - t0, js, bc, fn);
                ljs = js; lbc = bc; lfn = fn;
            }
            msleep(20);
        }
        n += snprintf(b + n, sizeof(b) - n, "  shot %d captured=%s\n",
                      shot + 1, (get_shooting_card()->file_number != fnb) ? "YES" : "NO");
    }
    FILE * f = FIO_CreateFile("ML/LOGS/CAPSIG.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(5000, "CAPSIG probe done -> ML/LOGS/CAPSIG.TXT");
}

/* PATH 1: test the NORMAL electronic shutter-press capture (SW1/SW2 via
 * PROP_REMOTE_SW1/SW2) instead of the slow IR-remote stopgap. The physical
 * shutter is fast AND self-finalizes (no saving-hang), so this path should be
 * too -- IF the R accepts the SW props. Fires ONE shot via SW1/SW2, logs the
 * job_state timeline + how fast it returns to idle -> ML/LOGS/SWCAP.TXT. Then
 * power off: clean = winner (fast + clean); saving-hang = SW path doesn't
 * finalize either. Compare timeline speed vs the IR-remote's ~1.7s grind. */
static void sw_capture_test_task()
{
    static char b[1600];
    int n = 0;
    gui_stop_menu(); msleep(800);
    int fn0 = get_shooting_card()->file_number;
    n += snprintf(b + n, sizeof(b) - n, "SW1/SW2 capture test\npre: js=0x%x file=%d\n",
                  lens_info.job_state, fn0);
    int t0 = get_ms_clock();
    SW1(1, 50);
    SW2(1, 250);
    SW2(0, 50);
    SW1(0, 50);
    n += snprintf(b + n, sizeof(b) - n, "SW seq done at t=%d ms\njob_state timeline:\n",
                  get_ms_clock() - t0);
    int last = -1;
    for (int k = 0; k < 90 && n < (int)sizeof(b) - 40; k++)   /* ~3s */
    {
        int js = lens_info.job_state;
        if (js != last)
        {
            n += snprintf(b + n, sizeof(b) - n, " [%d]0x%x", get_ms_clock() - t0, js);
            last = js;
        }
        msleep(33);
    }
    int captured = (get_shooting_card()->file_number != fn0);
    n += snprintf(b + n, sizeof(b) - n, "\ncaptured=%s\n", captured ? "YES" : "NO");
    FILE * f = FIO_CreateFile("ML/LOGS/SWCAP.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(6000, "SW capture: %s -> SWCAP.TXT. Power off: clean or hang?",
              captured ? "CAPTURED" : "no capture");
}

/* PATH 1: test direct capture commands found in the ROM, looking for a path that
 * is BOTH fast and self-finalizing (unlike the IR-remote which is clean but ~1.7s,
 * and call("Release") which is fast but hangs at power-off). Tries, in order:
 *   1. call("FA_Release")            -- direct release (the 40D's capture command)
 *   2. call("FA_RemoteRelease") + call("FA_FinishRemoteRelease")  -- release + explicit finalize
 * Each is a SEPARATE menu item so one capture is tested at a time. Logs the
 * job_state timeline + whether it captured. Then power off: clean = winner. */
static void fa_release_test_task()
{
    static char b[1500];
    int n = 0;
    gui_stop_menu(); msleep(800);
    int fn0 = get_shooting_card()->file_number;
    n += snprintf(b + n, sizeof(b) - n, "call(FA_Release) test\npre: js=0x%x file=%d\n",
                  lens_info.job_state, fn0);
    int t0 = get_ms_clock();
    call("FA_Release");
    n += snprintf(b + n, sizeof(b) - n, "call done at t=%d ms\njob_state:\n", get_ms_clock() - t0);
    int last = -1;
    for (int k = 0; k < 90 && n < (int)sizeof(b) - 40; k++)   /* ~3s */
    {
        int js = lens_info.job_state;
        if (js != last)
        {
            n += snprintf(b + n, sizeof(b) - n, " [%d]0x%x", get_ms_clock() - t0, js);
            last = js;
        }
        msleep(33);
    }
    int captured = (get_shooting_card()->file_number != fn0);
    n += snprintf(b + n, sizeof(b) - n, "\ncaptured=%s\n", captured ? "YES" : "NO");
    FILE * f = FIO_CreateFile("ML/LOGS/FACAP.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(6000, "FA_Release: %s -> FACAP.TXT. Power off: clean or hang?",
              captured ? "CAPTURED" : "no capture");
}

static void fa_remote_finish_test_task()
{
    static char b[1500];
    int n = 0;
    gui_stop_menu(); msleep(800);
    int fn0 = get_shooting_card()->file_number;
    n += snprintf(b + n, sizeof(b) - n, "FA_RemoteRelease + FA_FinishRemoteRelease test\npre: js=0x%x file=%d\n",
                  lens_info.job_state, fn0);
    int t0 = get_ms_clock();
    call("FA_RemoteRelease");
    msleep(50);
    call("FA_FinishRemoteRelease");
    n += snprintf(b + n, sizeof(b) - n, "calls done at t=%d ms\njob_state:\n", get_ms_clock() - t0);
    int last = -1;
    for (int k = 0; k < 90 && n < (int)sizeof(b) - 40; k++)
    {
        int js = lens_info.job_state;
        if (js != last)
        {
            n += snprintf(b + n, sizeof(b) - n, " [%d]0x%x", get_ms_clock() - t0, js);
            last = js;
        }
        msleep(33);
    }
    int captured = (get_shooting_card()->file_number != fn0);
    n += snprintf(b + n, sizeof(b) - n, "\ncaptured=%s\n", captured ? "YES" : "NO");
    FILE * f = FIO_CreateFile("ML/LOGS/FARMCAP.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(6000, "FA_Remote+Finish: %s -> FARMCAP.TXT. Power off: clean or hang?",
              captured ? "CAPTURED" : "no capture");
}

/* PATH 1: the key fast-capture experiment. The IR-remote path is clean but waits
 * ~1.7s (the develop) before it's "ready". The R bursts at 8fps because it
 * PIPELINES shots into a buffer instead of waiting. So: fire 5 IR-remote releases
 * only ~300ms apart -- much faster than develop -- and see how many actually
 * capture. If the buffer absorbs them (most/all fire), fast bracketing is possible
 * (we just wait for buffer space, not full develop). If they drop, the camera
 * genuinely can't accept the next yet via this path. Set CONTINUOUS drive first.
 * Logs per-shot job_state + capture -> ML/LOGS/RAPID.TXT. */
static void rapid_fire_test_task()
{
    static char b[2200];
    int n = 0;
    gui_stop_menu(); msleep(800);
    void (*ir_remote_release)(int) = (void *)0xE0190215u;
    int fn0 = get_shooting_card()->file_number;
    n += snprintf(b + n, sizeof(b) - n, "Rapid-fire: 5 releases ~300ms apart\npre: file=%d js=0x%x\n",
                  fn0, lens_info.job_state);
    int t0 = get_ms_clock();
    for (int shot = 0; shot < 5; shot++)
    {
        int fnb = get_shooting_card()->file_number;
        ir_remote_release(1); msleep(100); ir_remote_release(0);   /* quick press */
        n += snprintf(b + n, sizeof(b) - n, "shot %d @%dms js=0x%x", shot + 1,
                      get_ms_clock() - t0, lens_info.job_state);
        msleep(200);                                               /* short gap -- faster than develop */
        int fn = get_shooting_card()->file_number;
        n += snprintf(b + n, sizeof(b) - n, " -> js=0x%x file=%d %s\n",
                      lens_info.job_state, fn, (fn != fnb) ? "CAP" : "(no file yet)");
    }
    msleep(4000);                                                  /* let the buffer drain/develop */
    int total = get_shooting_card()->file_number - fn0;
    n += snprintf(b + n, sizeof(b) - n, "AFTER DRAIN: final file=%d -> %d of 5 captured\n",
                  get_shooting_card()->file_number, total);
    FILE * f = FIO_CreateFile("ML/LOGS/RAPID.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(7000, "Rapid-fire: %d of 5 captured -> RAPID.TXT", total);
}

/* PATH 2: dump the serial-flash driver struct. On the CAMERA the SF init succeeds
 * (unlike qemu), so the struct is fully populated -- giving the device constants
 * needed to enable qemu's serial_flash.c: SIO channel (+0x30), CS register pointer
 * (+0x2c), base (+0x14), size (+0x18), init flag (+0x10). -> ML/LOGS/SFSTRUCT.TXT. */
static void sf_struct_dump_task()
{
    static char b[1400];
    int n = 0;
    gui_stop_menu(); msleep(400);
    uint32_t s0 = MEM(0xE03C0358), s1 = MEM(0xE03C14B4), s2 = MEM(0xE03C1F90);
    n += snprintf(b + n, sizeof(b) - n, "SF globals: DAT_e03c0358=0x%X DAT_e03c14b4=0x%X DAT_e03c1f90=0x%X\n",
                  s0, s1, s2);
    uint32_t s = s2;   /* IsAddressSerialFlash struct: base/size/CS live here */
    if (s >= 0x1000 && s < 0x40000000)
    {
        n += snprintf(b + n, sizeof(b) - n,
                      "struct@0x%X: init[+10]=0x%X base[+14]=0x%X size[+18]=0x%X csptr[+2c]=0x%X ch[+30]=0x%X\n",
                      s, MEM(s + 0x10), MEM(s + 0x14), MEM(s + 0x18), MEM(s + 0x2c), MEM(s + 0x30));
        uint32_t csptr = MEM(s + 0x2c);
        if (csptr >= 0x1000 && csptr < 0xE0000000)
            n += snprintf(b + n, sizeof(b) - n, "CS register = 0x%X (current val=0x%X)\n", csptr, MEM(csptr));
        n += snprintf(b + n, sizeof(b) - n, "full struct:\n");
        for (uint32_t a = s; a < s + 0x80 && n < (int)sizeof(b) - 60; a += 16)
        {
            n += snprintf(b + n, sizeof(b) - n, "%08X:", a);
            for (int i = 0; i < 16; i += 4)
                n += snprintf(b + n, sizeof(b) - n, " %08X", MEM(a + i));
            n += snprintf(b + n, sizeof(b) - n, "\n");
        }
    }
    FILE * f = FIO_CreateFile("ML/LOGS/SFSTRUCT.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(4000, "SF struct dumped -> SFSTRUCT.TXT");
}

/* PATH 1: validate the fast-capture gate. RAPID.TXT showed the camera re-accepts a
 * shot ~700ms after the previous (not the ~1.7s full develop), and a dropped release
 * makes no file + does no harm. So: fire, and if no new file appeared, RETRY until it
 * takes -- this auto-paces at the camera's true accept rate. This test runs that gate
 * for 5 frames and reports per-frame time + attempts + total. Expect 5/5, ~700ms each.
 * CONTINUOUS drive, M mode. -> ML/LOGS/FASTFIRE.TXT. */
static void fastfire_retry_test_task()
{
    static char b[2000];
    int n = 0;
    gui_stop_menu(); msleep(800);
    void (*ir_remote_release)(int) = (void *)0xE0190215u;
    int fn0 = get_shooting_card()->file_number;
    n += snprintf(b + n, sizeof(b) - n, "Fast-fire RETRY: 5 frames, fire-until-accepted\npre file=%d\n", fn0);
    int big_t0 = get_ms_clock();
    for (int frame = 0; frame < 5; frame++)
    {
        int fnb = get_shooting_card()->file_number;
        int t0 = get_ms_clock();
        int attempts = 0, got = 0;
        while (get_ms_clock() - t0 < 4000 && !got)
        {
            attempts++;
            ir_remote_release(1); msleep(80); ir_remote_release(0);
            for (int i = 0; i < 15; i++)   /* ~300ms for the file to register */
            {
                if (get_shooting_card()->file_number != fnb) { got = 1; break; }
                msleep(20);
            }
            if (!got) msleep(80);          /* still busy -- brief wait then retry */
        }
        n += snprintf(b + n, sizeof(b) - n, "frame %d: %s in %dms, %d attempts (js=0x%x)\n",
                      frame + 1, got ? "CAP" : "FAIL", get_ms_clock() - t0, attempts, lens_info.job_state);
    }
    msleep(2000);
    int total = get_shooting_card()->file_number - fn0;
    int dt = get_ms_clock() - big_t0;
    n += snprintf(b + n, sizeof(b) - n, "TOTAL: %d of 5 in %dms (~%dms/frame)\n", total, dt, dt / 5);
    FILE * f = FIO_CreateFile("ML/LOGS/FASTFIRE.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(7000, "Fast-fire: %d of 5, ~%dms/frame", total, dt / 5);
}

/* PATH 1 (deeper layer): the retry gate gives ~700ms/frame because each release is a
 * separate CC remote-release sequence. The camera bursts at 8fps (~125ms) only when the
 * shutter is HELD and it stays in burst mode. So: hold the IR release down (press, no
 * release) for 1.5s in CONTINUOUS drive and count frames. If it bursts (~12 frames),
 * the true-rate path is reachable; for bracketing we'd then change exposure between the
 * buffered frames. -> ML/LOGS/BURST.TXT. */
static void burst_hold_test_task()
{
    static char b[1500];
    int n = 0;
    gui_stop_menu(); msleep(800);
    void (*ir_remote_release)(int) = (void *)0xE0190215u;
    int fn0 = get_shooting_card()->file_number;
    n += snprintf(b + n, sizeof(b) - n, "Burst HOLD test: hold release 1500ms (CONTINUOUS drive)\npre file=%d\n", fn0);
    int t0 = get_ms_clock();
    ir_remote_release(1);                 /* PRESS + HOLD */
    int last = fn0;
    for (int k = 0; k < 30; k++)          /* 1500ms */
    {
        int fn = get_shooting_card()->file_number;
        if (fn != last && n < (int)sizeof(b) - 30)
        {
            n += snprintf(b + n, sizeof(b) - n, " [%dms]f=%d", get_ms_clock() - t0, fn);
            last = fn;
        }
        msleep(50);
    }
    ir_remote_release(0);                 /* RELEASE */
    int held = get_shooting_card()->file_number - fn0;
    msleep(2500);                         /* let the buffer drain */
    int total = get_shooting_card()->file_number - fn0;
    int msper = (total > 0) ? 1500 / total : 0;
    n += snprintf(b + n, sizeof(b) - n, "\nduring hold: %d frames; after drain: %d total; ~%d ms/frame (8fps=125ms)\n",
                  held, total, msper);
    FILE * f = FIO_CreateFile("ML/LOGS/BURST.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(7000, "Burst: %d frames / 1.5s hold (~%dms/frame)", total, msper);
}

/* PATH 2: call the firmware's own full serial-flash dump (FUN_e03c052a). It acquires
 * the SF, reads all 8MB, and writes it to card files -- giving us the SF DATA the qemu
 * loaders hang without. EXPERIMENTAL: calling an internal factory function directly; if
 * it hangs, pull the battery (no harm). Run once; then check the card root for new .bin
 * files. Also wakes the SF, so a struct dump right after would show the live channel/CS. */
static void sf_firmware_dump_task()
{
    gui_stop_menu(); msleep(500);
    NotifyBox(3000, "Calling firmware SF dump (8MB)... wait ~30s");
    msleep(1500);
    void (*sf_full_dump)(void) = (void *)0xE03C052Bu;   /* FUN_e03c052a | 1 (thumb) */
    sf_full_dump();
    /* if we get here it returned cleanly; dump the now-active struct too */
    static char b[700];
    int n = 0;
    uint32_t s = MEM(0xE03C1F90);
    n += snprintf(b + n, sizeof(b) - n, "after firmware SF dump: struct@0x%X init=0x%X ch=0x%X csptr=0x%X size=0x%X base=0x%X\n",
                  s, MEM(s + 0x10), MEM(s + 0x30), MEM(s + 0x2c), MEM(s + 0x18), MEM(s + 0x14));
    FILE * f = FIO_CreateFile("ML/LOGS/SFDUMP.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(8000, "SF dump returned. Look for .bin files on card root + SFDUMP.TXT");
}

/* Dump the secondary-ROM / FROM region (0xF0000000+) that holds the property
 * tuning data the qemu boot reads as garbage (random ROM1 placeholder). The
 * firmware loads property packages from 0xF09C0000 (TUNE/0x02), 0xF0A80000,
 * 0xF0AC0000 (Main/StartupDataLoad.c). First write a readability check
 * (ROM1CHK.TXT) -- if these are CPU-readable (non-zero), dump 16MB via plain
 * MEM() reads (no SPI, so no sf_dump-style SIO deadlock) -> ML/LOGS/ROM1.BIN. */
static void rom1_dump_task()
{
    static uint8_t buf[0x4000];  /* chunk buffer (kept small to limit BSS) */
    char cb[400]; int cn = 0;
    const uint32_t probes[] = {0xF0000000, 0xF09C0000, 0xF0A80000, 0xF0AC0000};
    gui_stop_menu(); msleep(400);
    cn += snprintf(cb + cn, sizeof(cb) - cn, "ROM1 readability check:\n");
    for (int i = 0; i < 4; i++) {
        cn += snprintf(cb + cn, sizeof(cb) - cn, "%08X: %08X %08X %08X %08X\n",
                       probes[i], MEM(probes[i]), MEM(probes[i] + 4),
                       MEM(probes[i] + 8), MEM(probes[i] + 12));
    }
    FILE * c = FIO_CreateFile("ML/LOGS/ROM1CHK.TXT");
    if (c) { FIO_WriteFile(c, cb, cn); FIO_CloseFile(c); }

    /* gate on the ROM1 base (0xF0000000 = 0x80000424 header + "7.3.9"); the
     * region is fully mapped & readable even where blank (returns 0xFF), so a
     * 16MB read is safe. */
    uint32_t w0 = MEM(0xF0000000);
    if (w0 == 0 || w0 == 0xFFFFFFFF) return;   /* not memory-mapped here -> needs SPI path */

    FILE * f = FIO_CreateFile("ML/LOGS/ROM1.BIN");
    if (!f) return;
    for (uint32_t off = 0; off < 0x1000000; off += sizeof(buf)) {   /* 16MB */
        volatile uint32_t * src = (volatile uint32_t *)(0xF0000000u + off);
        uint32_t * dst = (uint32_t *)buf;
        for (unsigned i = 0; i < sizeof(buf) / 4; i++) dst[i] = src[i];
        FIO_WriteFile(f, buf, sizeof(buf));
    }
    FIO_CloseFile(f);
}
#endif

#ifdef FEATURE_BOOTFLAG_MENU
static void bootflag_disable(void* priv, int delta)
{
    console_show();
    printf("Call DisableBootDisk()\n");

    call("DisableBootDisk");

    printf("done.\n");
}

static void bootflag_enable(void* priv, int delta)
{
  console_show();
  printf("Call EnableBootDisk()\n");

  call("EnableBootDisk");

  printf("done.\n");
}
#endif

static void unmount_sd_card()
{
    extern void FSUunMountDevice(int drive);
    
    msleep(1000);
    console_clear();
    console_show();
    
    /* call shutdown hooks that need to save configs */
    config_save_at_shutdown();

#if defined(CONFIG_MODULES)
    extern int module_shutdown();
    module_shutdown();
#endif
    
    /* unmount the SD card */
    FSUunMountDevice(2);
    
    printf("Unmounted SD card.\n");
    printf("You may now copy files remotely on your wifi card.\n");
    printf("Press shutter halfway to reboot.\n");
    
    while (!get_halfshutter_pressed())
    {
        info_led_on();
        msleep(10);
    }

    int reboot = 0;
    prop_request_change(PROP_REBOOT, &reboot, 4);
}

#if CONFIG_DEBUGMSG
static void dbg_draw_props(int changed);
static unsigned dbg_last_changed_propindex = 0;

void
memfilt(void* m, void* M, int value)
{
    int k = 0;
    bmp_printf(FONT_SMALL, 0, 0, "%8x", value);
    for (void* i = m; i < M; i ++)
    {
        if ((*(uint8_t*)i) == value)
        {
            int x =  10 + 4 * 22 * (k % 8);
            int y =  10 + 12 * (k / 8);
            bmp_printf(FONT_SMALL, x, y, "%8x", i);
            k = (k + 1) % 240;
        }
    }
    int x =  10 + 4 * 22 * (k % 8);
    int y =  10 + 12 * (k / 8);
    bmp_printf(FONT_SMALL, x, y, "        ");
}
#endif

static int screenshot_sec = 0;


#ifdef CONFIG_HEXDUMP

CONFIG_INT("hexdump", hexdump_addr, 0x24298);

int hexdump_enabled = 0;

static MENU_UPDATE_FUNC (hexdump_print_value_hex)
{
    MENU_SET_VALUE("0x%x",
        MEMX(hexdump_addr)
    );
}

static MENU_UPDATE_FUNC (hexdump_print_value_int32)
{
    MENU_SET_VALUE(
        "%d",
        MEMX(hexdump_addr)
    );
}

static MENU_UPDATE_FUNC (hexdump_print_value_int16)
{
    int value = MEMX(hexdump_addr);
    MENU_SET_VALUE(
        "%d %d",
        value & 0xFFFF, (value>>16) & 0xFFFF
    );
}

static MENU_UPDATE_FUNC (hexdump_print_value_int8)
{
    int value = MEMX(hexdump_addr);
    MENU_SET_VALUE(
        "%d %d %d %d",
        (int8_t)( value      & 0xFF),
        (int8_t)((value>>8 ) & 0xFF),
        (int8_t)((value>>16) & 0xFF),
        (int8_t)((value>>24) & 0xFF)
    );
}

static MENU_UPDATE_FUNC (hexdump_print_value_str)
{
    if (hexdump_addr & 0xF0000000) return;
    MENU_SET_VALUE(
        "%s",
        (char*)hexdump_addr
    );
}

static void
hexdump_toggle_value_int32(void * priv, int delta)
{
    MEM(hexdump_addr) += delta;
}

static void
hexdump_toggle_value_int16(void * priv, int delta)
{
    (*(int16_t*)(hexdump_addr+2)) += delta;
}

int hexdump_prev = 0;
void hexdump_back(void* priv, int dir)
{
    hexdump_addr = hexdump_prev;
}
void hexdump_deref(void* priv, int dir)
{
    if (dir < 0) hexdump_back(priv, dir);
    hexdump_prev = hexdump_addr;
    hexdump_addr = MEMX(hexdump_addr);
}
#endif

static int crash_log_requested = 0;
void request_crash_log(int type)
{
    crash_log_requested = type;
}

static int core_dump_requested = 0;
static int core_dump_req_from = 0;
static int core_dump_req_size = 0;
void request_core_dump(int from, int size)
{
    core_dump_req_from = from;
    core_dump_req_size = size;
    core_dump_requested = 1;
}

extern int GetFreeMemForAllocateMemory();

#ifdef CONFIG_CRASH_LOG
static void save_crash_log()
{
    static char log_filename[100];

    int log_number = 0;
    for (log_number = 0; log_number < 100; log_number++)
    {
        snprintf(log_filename, sizeof(log_filename), crash_log_requested == 1 ? "CRASH%02d.LOG" : "ASSERT%02d.LOG", log_number);
        uint32_t size;
        if( FIO_GetFileSize( log_filename, &size ) != 0 ) break;
        if (size == 0) break;
    }

    FILE* f = FIO_CreateFile(log_filename);
    if (f)
    {
        my_fprintf(f, "%s\n", get_assert_msg());
        my_fprintf(f,
            "Magic Lantern version: %s\n"
            "Git commit: %s\n"
            "Built on %s by %s.\n",
            build_version,
            build_id,
            build_date,
            build_user);

        int M = GetFreeMemForAllocateMemory();
        int m = GetFreeMemForMalloc();
        my_fprintf(f,
            "Free Memory  : %dK + %dK\n",
            m/1024, M/1024
        );

        FIO_CloseFile(f);
    }

    msleep(1000);

    if (crash_log_requested == 1)
    {
        NotifyBox(5000, "Crash detected - log file saved.\n"
                        "Pls send CRASH%02d.LOG to ML devs.\n"
                        "\n"
                        "%s", log_number, get_assert_msg());
    }
    else
    {
        printf("%s\n", get_assert_msg());
        console_show();
    }

}

static void crash_log_step()
{
    static int dmlog_saved = 0;
    if (crash_log_requested)
    {
        //~ beep();
        save_crash_log();
        crash_log_requested = 0;
        msleep(2000);
    }

    if (core_dump_requested)
    {
        NotifyBox(100000, "Saving core dump, please wait...\n");
        save_mem_to_file((void*)core_dump_req_from, core_dump_req_from + core_dump_req_size, "COREDUMP.DAT");
        NotifyBox(10000, "Pls send COREDUMP.DAT to ML devs.\n");
        core_dump_requested = 0;
    }

    //~ bmp_printf(FONT_MED, 100, 100, "%x ", get_current_dialog_handler());
    extern thunk ErrForCamera_handler;
    if (get_current_dialog_handler() == &ErrForCamera_handler)
    {
        if (!dmlog_saved)
        {
            beep();
            NotifyBox(10000, "Saving debug log...");
            call("dumpf");
        }
        dmlog_saved = 1;
    }
    else dmlog_saved = 0;
}
#endif

static void
debug_loop_task( void* unused ) // screenshot, draw_prop
{
    TASK_LOOP
    {
#ifdef CONFIG_HEXDUMP
        if (hexdump_enabled)
            bmp_hexdump(FONT_SMALL, 0, 480-120, (void*) hexdump_addr, 32*10);
#endif

        #ifdef FEATURE_SCREENSHOT
        if (screenshot_sec)
        {
            info_led_blink(1, 20, 1000-20-200);
            screenshot_sec--;
            if (!screenshot_sec)
                take_screenshot(SCREENSHOT_FILENAME_AUTO, SCREENSHOT_BMP | SCREENSHOT_YUV);
        }
        #endif

        #ifdef CONFIG_RESTORE_AFTER_FORMAT
        if (MENU_MODE)
        {
            HijackFormatDialogBox_main();
        }
        #endif

        #if CONFIG_DEBUGMSG
        if (draw_prop)
        {
            dbg_draw_props(dbg_last_changed_propindex);
            continue;
        }
        #endif

        #ifdef CONFIG_CRASH_LOG
        crash_log_step();
        #endif

        msleep(200);
    }
}

static void screenshot_start(void* priv, int delta)
{
    screenshot_sec = 10;
}

#ifdef FEATURE_SHOW_IMAGE_BUFFERS_INFO
static MENU_UPDATE_FUNC(image_buf_display)
{
    MENU_SET_VALUE(
        "%dx%d, %dx%d",
        vram_lv.width, vram_lv.height,
        vram_hd.width, vram_hd.height
    );
}
#endif

static MENU_UPDATE_FUNC(shuttercount_display)
{
#if defined(CONFIG_DIGIC_8X)
    // just shutter count value
    MENU_SET_VALUE("%d", shutter_count);
#else
    MENU_SET_VALUE(
        "%dK = %d+%d",
        (shutter_count_plus_lv_actuations + 500) / 1000,
        shutter_count, shutter_count_plus_lv_actuations - shutter_count
    );
#endif

    if (shutter_count_plus_lv_actuations > CANON_SHUTTER_RATING*2)
    {
        MENU_SET_WARNING(MENU_WARN_ADVICE, "Lets break Guiness World Records (rated lifespan %d).", CANON_SHUTTER_RATING);
    }
    else if (shutter_count_plus_lv_actuations > CANON_SHUTTER_RATING)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "Lifespans are for wimps (rated lifespan %d).", CANON_SHUTTER_RATING);
    }
    else if (shutter_count_plus_lv_actuations > CANON_SHUTTER_RATING/2)
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "I hope I get to rated lifespan (rated lifespan %d).", CANON_SHUTTER_RATING);
    }
    else
    {
        MENU_SET_WARNING(MENU_WARN_INFO, "You may get around %d.", CANON_SHUTTER_RATING);
    }
}

#if defined(CONFIG_DIGIC_8X)
static MENU_UPDATE_FUNC(totalshutter_display)
{
    MENU_SET_VALUE("%d", shutter_count);
    MENU_SET_WARNING(
        MENU_WARN_ADVICE,
        "May drift a bit - check after restart for accurate values.");
}
static MENU_UPDATE_FUNC(totalmirror_display)
{
    MENU_SET_VALUE("%d", total_mirror_count);
    MENU_SET_WARNING(
        MENU_WARN_ADVICE,
        "May drift a bit - check after restart for accurate values.");
}
static MENU_UPDATE_FUNC(totalshoot_display)
{
    MENU_SET_VALUE("%d", total_shots_count);
    MENU_SET_WARNING(
        MENU_WARN_ADVICE,
        "May drift a bit - check after restart for accurate values.");
}
#endif

#ifdef FEATURE_SHOW_CMOS_TEMPERATURE
#ifdef EFIC_CELSIUS
#define FAHRENHEIT (EFIC_CELSIUS * 9 / 5 + 32)
static MENU_UPDATE_FUNC(efictemp_display)
{
    MENU_SET_VALUE(
        "%d C, %d F, %d raw",
        EFIC_CELSIUS, FAHRENHEIT, efic_temp
    );
}
#else
static MENU_UPDATE_FUNC(efictemp_display)
{
    MENU_SET_VALUE(
        "%d raw (help needed)",
        efic_temp
    );
}
#endif
#endif

#if 0 // CONFIG_5D2
static void ambient_display(
    void *            priv,
    int            x,
    int            y,
    int            selected
)
{
    extern int lightsensor_raw_value;
    int ev = gain_to_ev_scaled(lightsensor_raw_value, 10);
    bmp_printf(
        selected ? MENU_FONT_SEL : MENU_FONT,
        x, y,
        "Ambient light: %d.%d EV",
        ev/10, ev%10
    );
    menu_draw_icon(x, y, MNI_ON, 0);
}
#endif

#ifdef CONFIG_R
/* ---- METERING PROBE (Debug -> "Brightness probe"). Find a scene-brightness signal on the R. v1
 * (PROP_BV via an M-mode half-press) never updated -- the R doesn't surface brightness in M mode.
 * v2 watches the AUTO/metering properties the camera computes WHEN IT METERS:
 *   SA = PROP_SHUTTER_AUTO   (the shutter it PICKS in Av mode -- the prime candidate)
 *   IA = PROP_ISO_AUTO,  AA = PROP_APERTURE_AUTO,  LV = PROP_LV_BV (LiveView brightness),  BV = PROP_BV
 * Runs ~2 min, half-pressing to meter each line. *** RUN IN Av MODE (set a fixed aperture) and vary
 * the light. *** Whichever value TRACKS the light is our signal -- SA should: brighter scene -> faster
 * picked shutter. -> ML/LOGS/BRIGHT.TXT */
static const unsigned meter_props[] = {
    PROP_SHUTTER, PROP_SHUTTER_AUTO, PROP_ISO_AUTO, PROP_AE, PROP_LV_BV, PROP_BV
};
#define METER_NPROP ((int)(sizeof(meter_props)/sizeof(meter_props[0])))
static void *          meter_token = NULL;
static volatile uint32_t meter_val[METER_NPROP];
static volatile uint32_t meter_seq[METER_NPROP];
static volatile int      meter_active = 0;
static void meter_token_handler(void * token) { meter_token = token; }
static void * meter_cb(unsigned property, void * priv, void * addr, unsigned len)
{
    extern void* _prop_cleanup(void* token, int property);
    for (int i = 0; i < METER_NPROP; i++)
    {
        if (property == meter_props[i] && addr)
        {
            uint32_t w = 0;
            unsigned c = len < 4 ? len : 4;
            for (unsigned j = 0; j < c; j++) ((uint8_t *)&w)[j] = ((uint8_t *)addr)[j];
            meter_val[i] = w; meter_seq[i]++;
        }
    }
    return (void *)_prop_cleanup(meter_token, (int)property);
}
static void meter_start(void)
{
    if (meter_active) return;
    meter_active = 1;
    prop_register_slave((unsigned *)meter_props, METER_NPROP, meter_cb, NULL, meter_token_handler);
}
static void brightness_probe_task(void)
{
    static char b[10000]; int n = 0;
    gui_stop_menu();
    msleep(500);
    meter_start();
    n += snprintf(b + n, sizeof(b) - n,
        "RUN IN LIVEVIEW + vary light. avgY = average luma of the LIVE IMAGE (0-255) = the real scene "
        "brightness, straight off the sensor feed. lv=buffer w x h. LV_BV=PROP_LV_BV (backup).\n");
    for (int i = 0; i < 90 && n < (int)sizeof(b) - 90; i++)  /* ~2.5 min */
    {
        msleep(1600);
        /* average the luma of the LiveView YUV422 image -- this is the scene brightness itself,
         * independent of the (uncooperative) meter. UYVY: Y in bytes 1 and 3 of each 32-bit word. */
        int avg_y = -1, w = 0, h = 0;
        struct vram_info * lv = get_yuv422_vram();
        if (lv) { w = lv->width; h = lv->height; }
        if (lv && lv->vram && lv->pitch > 0 && lv->height > 0)
        {
            const uint32_t * buf = (const uint32_t *)lv->vram;
            int n32 = (lv->pitch * lv->height) / 4;
            long sum = 0; int s = 0;
            for (int p = 0; p < n32; p += 97)   /* sparse prime-ish stride */
            {
                uint32_t px = buf[p];
                sum += ((((px >> 24) & 0xFF) + ((px >> 8) & 0xFF)) >> 1);   /* avg of the 2 Y's */
                s++;
            }
            if (s) avg_y = (int)(sum / s);
        }
        n += snprintf(b + n, sizeof(b) - n,
            "%d avgY=%d  (lv %dx%d)  LV_BV=0x%x/%d\n",
            i, avg_y, w, h, (unsigned)meter_val[4], (int)meter_seq[4]);
        FILE * f = FIO_CreateFile("ML/LOGS/BRIGHT.TXT");
        if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    }
    NotifyBox(3000, "LiveView luma probe done -> BRIGHT.TXT");
}

/* ---- RAW BUFFER HOOK, Phase 1 diagnostic (Debug -> "Arm raw hook").
 * The 14-bit raw channel's register block (0xD0487xxx) hard-faults on direct CPU read, so we cannot
 * read its +0xa0 buffer pointer. Instead we runtime-hook FUN_e05364b6 @0xE05364B6 -- the per-frame leaf
 * that sets EVERY channel's buffer pointer: *(DmacInfo[chan].pBlock + 0xa0) = addr. The hook
 * (rawhk_wrapper) logs which channels get a buffer + the addr, then does the original write. SAFE: only
 * the readable DmacInfo TABLE is read; the +0xa0 store is the exact write Canon does here, in Canon's
 * powered context (no faulting read). Installed on-demand via convert_f_patch_to_patch()+apply_patches()
 * (NOT at boot, so a card pull recovers); needs the MMU 2-page bump (page 0xE0530000). Arm, RECORD 12s,
 * auto-unpatch + dump. Phase 1 confirms the raw channel index; Phase 2 will redirect it to ML's buffer.
 * -> ML/LOGS/RAWHK.TXT */
#define RAWHK_NCH 80
static volatile uint32_t rawhk_addr[RAWHK_NCH];   /* last non-zero +0xa0 buffer addr per channel */
static volatile uint32_t rawhk_hits[RAWHK_NCH];   /* call count per channel */
static volatile uint32_t rawhk_total;             /* all calls while patched -- proves the hook is live */
static volatile int      rawhk_on;

/* Replaces FUN_e05364b6 (a one-line leaf): *(DmacInfo[chan].pBlock + 0xa0) = addr.  r0=chan, r1=addr.
 * Log which channels get a buffer set + the addr (to find the RAW channel), then do the original write.
 * SAFE: reads only the readable DmacInfo TABLE (*(0xE0536D58)=0xE0DD5C64); the ONLY channel-register
 * touch is the +0xa0 WRITE -- the exact store Canon itself does here, in Canon's own powered context
 * (no faulting CPU read of 0xD0487xxx). The write always happens (preserves the original behaviour);
 * only the logging is gated on rawhk_on. */
void rawhk_wrapper(uint32_t chan, uint32_t addr);
void rawhk_wrapper(uint32_t chan, uint32_t addr)
{
    rawhk_total++;
    uint32_t base   = *(volatile uint32_t *)0xE0536D58;       /* DmacInfo base = 0xE0DD5C64 */
    uint32_t pblock = *(volatile uint32_t *)(base + chan * 8);
    if (rawhk_on && chan < RAWHK_NCH)
    {
        rawhk_hits[chan]++;
        if (addr) rawhk_addr[chan] = addr;                    /* keep last non-zero (teardown writes 0) */
    }
    *(volatile uint32_t *)(pblock + 0xa0) = addr;             /* replicate FUN_e05364b6 */
}

static void rawhk_task(void)
{
    gui_stop_menu();
    msleep(500);
    for (int i = 0; i < RAWHK_NCH; i++) { rawhk_addr[i] = 0; rawhk_hits[i] = 0; }
    rawhk_on = 0; rawhk_total = 0;

    /* runtime-hook FUN_e05364b6 @0xE05364B6 (page 0xE0530000 -- needs the MMU 2-page bump).
     * orig 8 bytes: 88 4a (ldr r2,[pc,..]); 52 f8 30 00 (ldr.w); c0 f8 (str.w r1,[r0,#0xa0]). */
    static struct function_hook_patch fhp;
    static struct patch p;
    static uint8_t hookmem[8];
    fhp.patch_addr = 0xE05364B6;
    static const uint8_t oc[8] = {0x88, 0x4a, 0x52, 0xf8, 0x30, 0x00, 0xc0, 0xf8};
    for (int i = 0; i < 8; i++) fhp.orig_content[i] = oc[i];
    fhp.target_function_addr = (uint32_t)&rawhk_wrapper;
    fhp.description = "raw +a0 cap";
    if (convert_f_patch_to_patch(&fhp, &p, hookmem))
    {
        NotifyBox(6000, "raw hook: convert_f_patch failed (no hook)");
        return;
    }
    int err = apply_patches(&p, 1);
    if (err)
    {
        NotifyBox(8000, "raw hook: apply_patches err=0x%x (no hook, safe)", (unsigned)err);
        return;
    }
    uint32_t patched = *(volatile uint32_t *)0xE05364B6;   /* should read back f000f8df (ldr.w pc,[pc]) */

    rawhk_on = 1;
    NotifyBox(13000, "RAW HOOK ARMED -- press REC and RECORD now (12s)!");
    beep();
    msleep(7000);   /* let every channel populate + recording stabilise */
    /* SNAPSHOT while recording (buffers fresh): the hook captured each channel's buffer addr via the
     * function ARG (r1) -- those are RAM addresses, readable even for the faulting-region channels whose
     * REGISTERS we can't read. Copy 128KB of each logged channel's buffer into one RAM blob (header per
     * chunk), to render on the PC and find the 14-bit raw. */
    void * blob = fio_malloc(0x400000u);   /* 4MB */
    uint32_t bn = 0;
    if (blob)
    {
        for (int c = 0; c < RAWHK_NCH && bn + 0x20000u + 16u <= 0x400000u; c++)
        {
            if (!rawhk_hits[c]) continue;
            uint32_t a0 = rawhk_addr[c];
            uint32_t cp = a0 & ~0x40000000u;
            if (cp < 0x01000000u || cp >= 0x60000000u) continue;
            uint32_t * hdr = (uint32_t *)((uint8_t *)blob + bn);
            hdr[0] = 0x52415748u; hdr[1] = (uint32_t)c; hdr[2] = a0; hdr[3] = 0x20000u;   /* magic,idx,a0,len */
            bn += 16;
            memcpy((uint8_t *)blob + bn, (void *)UNCACHEABLE(cp), 0x20000u);
            bn += 0x20000u;
        }
    }
    msleep(2000);
    rawhk_on = 0;
    msleep(50);
    unpatch_memory(0xE05364B6);
    if (blob)
    {
        FILE * bf = FIO_CreateFile("ML/LOGS/RAWHKB.BIN");
        if (bf) { for (uint32_t o = 0; o < bn; o += 0x10000u) { uint32_t cs = bn - o < 0x10000u ? bn - o : 0x10000u; FIO_WriteFile(bf, (uint8_t *)blob + o, cs); } FIO_CloseFile(bf); }
        fio_free(blob);
    }

    static char b[3600]; int n = 0;
    /* ML snprintf supports %d/%x/%08x only. */
    n += snprintf(b + n, sizeof(b) - n,
        "raw +0xa0 hook: total=%d  entry=%08x (hook ok if f000f8df)\n"
        "idx pblock hits a0 -- the RAW chan = a big rotating a0 (~0x4xxxxxxx) set every frame.\n",
        (int)rawhk_total, (unsigned)patched);
    uint32_t base = *(volatile uint32_t *)0xE0536D58;
    for (int c = 0; c < RAWHK_NCH && n < (int)sizeof(b) - 80; c++)
    {
        if (!rawhk_hits[c]) continue;
        uint32_t pblock = *(volatile uint32_t *)(base + c * 8);
        n += snprintf(b + n, sizeof(b) - n, "idx%d %08x hits=%d a0=%08x\n",
                      c, (unsigned)pblock, (int)rawhk_hits[c], (unsigned)rawhk_addr[c]);
    }
    FILE * f = FIO_CreateFile("ML/LOGS/RAWHK.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(9000, "raw hook: %d calls -> RAWHK.TXT", (int)rawhk_total);
}

/* ---- EXPERIMENTAL SLURP (Debug -> "Slurp raw").  Pull the sensor 14-bit raw into OUR buffer the
 * mlv_lite way: commandeer a FREE write EDMAC channel and connect it to the sensor raw SOURCE
 * (connection 0 -- qemu-eos engine.c: conn 0/35 = sensor 14-bit raw; raw_width=xb*8/14, raw_height=yb+1,
 * 14-bit packed). Sequence (R EDMAC API): set buffer (+0xa0 via FUN_e05364b6 0xE05364B6); SetEDmac
 * geometry (0xE0536ABC, port,addr,b14,edmac_info*; xb=ei[0x10], yb=ei[0x13], xn=yn=0 single block);
 * ConnectWriteEDmac(chan,0) (0xE053607C); StartEDmac (0xE053595E, +0xb4=1); wait during recording; stop
 * (0xE0536142, +0xb4=0); copy. chan/conn/geometry are EXPERIMENTAL -> iterate. Menu-invoked (recoverable).
 * -> ML/LOGS/SLURP.BIN + SLURP.TXT. */
static void slurp_raw_task(void)
{
    gui_stop_menu();
    msleep(300);
    void (*r_setbuf)(uint32_t, uint32_t)                        = (void *)(0xE05364B6u | 1);
    void (*r_setedmac)(uint32_t, uint32_t, uint32_t, uint32_t *) = (void *)(0xE0536ABCu | 1);
    void (*r_connw)(uint32_t, uint32_t)                         = (void *)(0xE053607Cu | 1);
    void (*r_start)(uint32_t)                                   = (void *)(0xE053595Eu | 1);
    void (*r_stop)(uint32_t)                                    = (void *)(0xE0536142u | 1);

    const uint32_t chan = 7, conn = 0;            /* idx7 = free write chan; conn 0 = sensor raw */
    const uint32_t W = 1920, H = 1080;
    uint32_t pitch = W * 14u / 8u;                /* 14-bit packed bytes/row */
    uint32_t sz = pitch * H;
    void * buf = fio_malloc(sz);
    if (!buf) { NotifyBox(6000, "slurp: fio_malloc %d failed", (int)sz); return; }
    uint32_t ubuf = (uint32_t)buf | 0x40000000u;   /* R uncacheable alias (mem_defs UNCACHEABLE) */
    memset((void *)ubuf, 0, sz);

    static uint32_t ei[0x16];
    for (int i = 0; i < 0x16; i++) ei[i] = 0;
    ei[0x10] = pitch;     /* xb = bytes/row */
    ei[0x13] = H - 1;     /* yb = height-1 (xn=yn=0 -> single contiguous block, skips asserts) */

    NotifyBox(15000, "SLURP idx7<-conn0 -- press REC + RECORD now (~10s)!");
    beep();
    msleep(6000);         /* let recording stabilise so the sensor raw source is hot */

    r_setbuf(chan, ubuf);          /* +0xa0 = our buffer */
    r_setedmac(chan, 0, 0, ei);    /* geometry */
    r_connw(chan, conn);           /* connect to sensor raw source */
    r_start(chan);                 /* start the DMA */
    msleep(400);                   /* ~12 frames */
    r_stop(chan);
    msleep(50);

    uint32_t pblock = *(volatile uint32_t *)(0xE0DD5C64u + chan * 8);
    static char t[320]; int n = 0;
    n += snprintf(t + n, sizeof(t) - n, "slurp idx%d conn%d %dx%d pitch%d sz%d pblock=%08x\nregs:",
                  (int)chan, (int)conn, (int)W, (int)H, (int)pitch, (int)sz, (unsigned)pblock);
    for (uint32_t off = 0x48; off <= 0x58; off += 4)
        n += snprintf(t + n, sizeof(t) - n, " %x=%08x", (unsigned)off, (unsigned)*(volatile uint32_t *)(pblock + off));
    n += snprintf(t + n, sizeof(t) - n, " a0=%08x b4=%08x buf=%08x\n",
                  (unsigned)*(volatile uint32_t *)(pblock + 0xa0u), (unsigned)*(volatile uint32_t *)(pblock + 0xb4u), (unsigned)ubuf);
    FILE * tf = FIO_CreateFile("ML/LOGS/SLURP.TXT");
    if (tf) { FIO_WriteFile(tf, t, n); FIO_CloseFile(tf); }

    FILE * f = FIO_CreateFile("ML/LOGS/SLURP.BIN");
    if (f) { for (uint32_t o = 0; o < sz; o += 0x10000u) { uint32_t cs = sz - o < 0x10000u ? sz - o : 0x10000u; FIO_WriteFile(f, (uint8_t *)ubuf + o, cs); } FIO_CloseFile(f); }
    fio_free(buf);
    msleep(1500);
    NotifyBox(12000, "slurp idx7 done -> SLURP.BIN (stop recording)");
}

/* ---- RAW BRIGHTNESS + SCREEN-SLEEP TEST (Debug -> "Raw bright test").
 * We found the LV raw write channel: DmacInfo Port 14 = pBlock 0xD0440000, buffer-pointer reg at
 * +0x50, content = uncompressed Bayer. This averages that buffer over ~80s while logging a checksum,
 * so we can answer the overnight-timelapse question: does the raw DMA keep WRITING when the display
 * sleeps? Run it, then let the screen sleep ~halfway. If chk keeps changing after screen-off, the raw
 * stream is display-independent (overnight-proof). avg = a brightness proxy (byte avg of packed raw).
 * Reads the live channel reg each tick (the channel is active in LV); if a screen-off fully stops LV
 * the reg read could fault -- the per-tick log persists, so the last entry still tells us when it died.
 * -> ML/LOGS/RAWBR.TXT */
extern void idle_wakeup_reset_counters(int reason);
/* ---- MULTI-OFFSET SCAN: find the LIVE buffer. The breadth scan only checked reg +0x50 and found all
 * frozen -- but the live buffer pointer can be at a DIFFERENT register offset (qemu-eos: EDMAC has many
 * regs; +0x08 is "RAM address" on old DIGIC). So for each CONFIRMED-SAFE channel (0xD0404..0xD045E; the
 * 0xD0487+ region faults) read MANY candidate offsets as buffer pointers, checksum each pointed buffer
 * TWICE ~60ms apart, and log only the ones that CHANGE frame-to-frame (= live) + a 16-byte content
 * sample so we can tell raw(Bayer/noise) from YUV(0x80 chroma). KEEP LV AWAKE (hold half-press) + VARY
 * THE SCENE. Safe: only reads confirmed-mapped channels + RAM-range buffer ptrs. -> ML/LOGS/RAWBR.TXT */
extern void idle_wakeup_reset_counters(int reason);
#define MULTISCAN_MAXC 420
static void raw_bright_task(void)
{
    static const uint32_t blk[] = {
        0xd0404000,
        0xd0420000,0xd0420100,0xd0420200,0xd0420300,0xd0420400,0xd0420500,0xd0420600,0xd0420700,
        0xd0420800,0xd0420900,0xd0420a00,0xd0420b00,0xd0420c00,
        0xd0440000,0xd0440100,0xd0440200,
        0xd045e000,0xd045e100,0xd045e200,
    };
    static const uint16_t offs[] = {0x08,0x0c,0x10,0x18,0x1c,0x20,0x28,0x2c,0x48,0x50,0x54,0x68,0x84,0xa0,0xa4,0xa8,0xac,0xb4,0xc0};
    static uint32_t cptr[MULTISCAN_MAXC], cka[MULTISCAN_MAXC];
    static uint16_t cblk[MULTISCAN_MAXC], coff[MULTISCAN_MAXC];
    const int nb = (int)(sizeof(blk)/sizeof(blk[0]));
    const int no = (int)(sizeof(offs)/sizeof(offs[0]));
    gui_stop_menu();
    msleep(300);
    static char b[12000]; int n = 0;
    n += snprintf(b + n, sizeof(b) - n,
        "MULTISCAN: %d safe chans x %d offsets as buf ptrs; uncached 16KB chk twice ~60ms apart.\n"
        "Only frame-CHANGING buffers logged + 16B sample. HOLD HALF-PRESS + VARY SCENE the whole time.\n", nb, no);
    for (int round = 0; round < 2 && n < (int)sizeof(b) - 600; round++)
    {
        idle_wakeup_reset_counters(-1);
        int nc = 0;
        for (int i = 0; i < nb; i++)
            for (int o = 0; o < no && nc < MULTISCAN_MAXC; o++)
            {
                uint32_t cp = *(volatile uint32_t *)(blk[i] + offs[o]) & ~0x40000000u;
                if (cp >= 0x01000000 && cp < 0x20000000)
                {
                    const volatile uint8_t * pu = (const volatile uint8_t *)UNCACHEABLE(cp);
                    uint32_t k = 0; for (int j = 0; j < 0x4000; j += 16) k = k * 31 + pu[j];
                    cblk[nc] = (uint16_t)i; coff[nc] = offs[o]; cptr[nc] = cp; cka[nc] = k; nc++;
                }
            }
        msleep(60);
        idle_wakeup_reset_counters(-1);
        int chg = 0;
        for (int c = 0; c < nc && n < (int)sizeof(b) - 140; c++)
        {
            const volatile uint8_t * pu = (const volatile uint8_t *)UNCACHEABLE(cptr[c]);
            uint32_t k = 0; for (int j = 0; j < 0x4000; j += 16) k = k * 31 + pu[j];
            if (k != cka[c])
            {
                chg++;
                n += snprintf(b + n, sizeof(b) - n, "CHG %08x+%x ptr=%08x kA=%08x kB=%08x mem:",
                    (unsigned)blk[cblk[c]], (unsigned)coff[c], (unsigned)cptr[c], (unsigned)cka[c], (unsigned)k);
                for (int j = 0; j < 16; j++) n += snprintf(b + n, sizeof(b) - n, " %02x", pu[j]);
                n += snprintf(b + n, sizeof(b) - n, "\n");
            }
        }
        n += snprintf(b + n, sizeof(b) - n, "round %d: %d ptrs, %d changing\n", round, nc, chg);
        FILE * f = FIO_CreateFile("ML/LOGS/RAWBR.TXT");
        if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    }
    FILE * f = FIO_CreateFile("ML/LOGS/RAWBR.TXT");
    if (f) { FIO_WriteFile(f, b, n); FIO_CloseFile(f); }
    NotifyBox(6000, "Multiscan done -> RAWBR.TXT");
}

/* ---- DUMP the P14 buffer to the card, so we can prove it's a real image (and read off geometry).
 * Works on the STATIC post-capture buffer (sidesteps the LV-sleep problem): TAKE A PHOTO first, then
 * run this. Reads the live ptr at P14 +0x50, dumps 4MB from there (uncached) to ML/LOGS/RAW.BIN.
 * Analyse on the PC: try widths as 14-bit packed Bayer; a coherent image = proof + the true width. */
static void raw_dump_task(void)
{
    gui_stop_menu();
    msleep(300);
    /* the LIVE channel found by the multiscan: 0xD0420700, buffer ptr at reg +0xa0 (rotating ring). */
    uint32_t ptr = *(volatile uint32_t *)(0xD0420700u + 0xa0u);
    uint32_t cp = ptr & ~0x40000000u;
    if (cp < 0x01000000 || cp >= 0x20000000)
    {
        NotifyBox(6000, "Raw dump: bad ptr %08x (LV not active?)", (unsigned)ptr);
        return;
    }
    uint32_t base = cp & ~0x000FFFFFu;   /* 1MB-align down to capture the frame/ring start */
    const uint8_t * src = (const uint8_t *)UNCACHEABLE(base);
    FILE * f = FIO_CreateFile("ML/LOGS/RAW.BIN");
    if (!f) { NotifyBox(6000, "Raw dump: cannot create RAW.BIN"); return; }
    uint32_t total = 0x800000;   /* 8 MB */
    for (uint32_t off = 0; off < total; off += 0x10000)
        FIO_WriteFile(f, src + off, 0x10000);
    FIO_CloseFile(f);
    /* self-document: dump the WHOLE channel register block 0x00..0xFC (safe -- active channel).
     * Key: +0x48 ys_xs, +0x54 yn_xn=(yn<<16)|xn -> width/height; +0xa0 ram_addr; +0xc0 transfer_mode;
     * +0xd0 PackUnpackInfo -> bit-depth/format. Non-zero regs only, to keep it compact. */
    static char m[1500]; int k = 0;
    k += snprintf(m + k, sizeof(m) - k, "RAW.BIN = 8MB from D0420700+0xa0; base=%08x live_ptr=%08x\nregs(nonzero):", (unsigned)base, (unsigned)ptr);
    for (int off = 0; off < 0x100 && k < (int)sizeof(m) - 20; off += 4)
    {
        uint32_t v = *(volatile uint32_t *)(0xD0420700u + off);
        if (v) k += snprintf(m + k, sizeof(m) - k, " %x=%08x", off, (unsigned)v);
    }
    k += snprintf(m + k, sizeof(m) - k, "\n");
    FILE * t = FIO_CreateFile("ML/LOGS/RAW.TXT");
    if (t) { FIO_WriteFile(t, m, k); FIO_CloseFile(t); }
    NotifyBox(8000, "Dumped 8MB @ %08x (ptr %08x) -> RAW.BIN", (unsigned)base, (unsigned)ptr);
}

/* ---- RAW CANDIDATE PROBE (Debug -> "Dump raw cands").  idx8 (0xD0420700) was DEBUNKED (a stats
 * buffer, not the raw).  The real sensor raw goes via DprawHeadToRaw/Mem1 -> a WIDE imaging-class
 * WRITE EDMAC channel.  The 200D/6D2 (DIGIC7 cousins) put it at index 23 ("Mem1 is Not Complete").
 * On R the wide (0x1c/0x1e transfer) imaging WRITE channels are: idx17/18 (0xD045E000/100, safe),
 * idx14 P14, and idx23/24 (0xD0487000/100) + idx26-28 (0xD04A2000-200) which FAULT on idle reads
 * (they power up only when the raw path runs).  Run this in MOVIE LiveView so they're powered.
 * Crash-bisect: LASTCH.TXT names the channel we're about to read (closed=flushed) so a fault on an
 * unpowered channel is identifiable after reboot. Each channel with a valid +0xa0 buffer -> 4MB dump. */
static void raw_cand_task(void)
{
    gui_stop_menu();
    msleep(300);
    static const uint32_t cand[] = {
        0xD045E000u, 0xD045E100u, 0xD0440000u,                            /* idx17,18,14 -- safe region */
        0xD0487000u, 0xD0487100u, 0xD04A2000u, 0xD04A2100u, 0xD04A2200u,  /* idx23,24,26,27,28 -- fault-risk */
    };
    FILE * t = FIO_CreateFile("ML/LOGS/RAWCAND.TXT");
    if (t) { const char * h = "raw candidate probe (wide imaging WRITE chans). RUN IN MOVIE LV.\n"; FIO_WriteFile(t, h, strlen(h)); }
    int dumped = 0;
    for (int i = 0; i < (int)(sizeof(cand) / sizeof(cand[0])); i++)
    {
        uint32_t ch = cand[i];
        /* ON-SCREEN crash indicator: survives a hard reboot (the SD markers didn't last time).
         * If the camera reboots, the LAST i/chan shown here is the faulting channel -- tell me that. */
        NotifyBox(2000, "Raw cand: i=%d chan %08x ...", i, (unsigned)ch);
        beep();
        msleep(900);
        /* crash-bisect marker -- create+close flushes it to SD BEFORE the risky MMIO read */
        FILE * mk = FIO_CreateFile("ML/LOGS/LASTCH.TXT");
        if (mk) { char b[48]; int n = snprintf(b, sizeof(b), "about to read chan %08x (i=%d)\n", (unsigned)ch, i); FIO_WriteFile(mk, b, n); FIO_CloseFile(mk); }
        /* potentially-faulting reads (channel register block) */
        uint32_t ptr = *(volatile uint32_t *)(ch + 0xa0u);
        uint32_t g50 = *(volatile uint32_t *)(ch + 0x50u);
        uint32_t g54 = *(volatile uint32_t *)(ch + 0x54u);
        uint32_t cp  = ptr & ~0x40000000u;
        char b[176]; int n = snprintf(b, sizeof(b), "i=%d chan %08x: a0=%08x 50=%08x 54=%08x", i, (unsigned)ch, (unsigned)ptr, (unsigned)g50, (unsigned)g54);
        if (cp >= 0x01000000u && cp < 0x60000000u)
        {
            char fn[28]; snprintf(fn, sizeof(fn), "ML/LOGS/RC%d.BIN", i);
            FILE * f = FIO_CreateFile(fn);
            if (f)
            {
                const uint8_t * src = (const uint8_t *)UNCACHEABLE(cp & ~0x000FFFFFu);
                for (uint32_t off = 0; off < 0x400000u; off += 0x10000u) FIO_WriteFile(f, src + off, 0x10000);
                FIO_CloseFile(f);
                n += snprintf(b + n, sizeof(b) - n, " -> RC%d.BIN 4MB @%08x", i, (unsigned)(cp & ~0x000FFFFFu));
                dumped++;
            }
        }
        else n += snprintf(b + n, sizeof(b) - n, " (no valid buffer)");
        n += snprintf(b + n, sizeof(b) - n, "\n");
        if (t) FIO_WriteFile(t, b, n);
    }
    if (t) FIO_CloseFile(t);
    FILE * mk = FIO_CreateFile("ML/LOGS/LASTCH.TXT");
    if (mk) { const char * d = "done -- no fault\n"; FIO_WriteFile(mk, d, strlen(d)); FIO_CloseFile(mk); }
    NotifyBox(10000, "Raw cand probe: %d chans dumped -> RC*.BIN", dumped);
}

/* ---- RECORD-GATED RAW CATCHER (Debug -> "Catch raw (REC)").  idx23 0xD0487000 is the Mem1/HeadToRaw
 * raw channel (matches the 200D/6D2 index-23); it FAULTS when idle and powers up only during recording
 * (confirmed: it faulted in plain Movie LV).  Arm this, then press the camera's REC button: it waits
 * for RECORDING_H264_STARTED, reads 0xD0487000+0xa0 (now powered), captures the buffer to RAM, and
 * after you stop recording writes RC487.BIN (+ RC487.TXT geometry) for the PC render. */
static void raw_catch_task(void)
{
    gui_stop_menu();
    msleep(300);
    NotifyBox(8000, "Raw catch ARMED -- press REC now (waiting 40s)");
    beep();
    /* gate on RECORDING_H264 (__recording>0) so we catch either "starting"(1) or "recording"(2);
     * the spin-up delay below then ensures the raw pipeline is fully live. Requires PROP_MVR_REC_START
     * to be allowed (un-denied in property_whitelist.h). */
    int waited = 0;
    while (!RECORDING_H264 && waited < 40000) { msleep(100); waited += 100; }
    if (!RECORDING_H264) { NotifyBox(6000, "Raw catch: no recording seen -> abort"); return; }
    beep();
    msleep(1500);   /* let the raw pipeline spin up so 0xD0487000 is powered */
    /* crash-visible: if this read still faults, the camera reboots right here */
    NotifyBox(3000, "Raw catch: reading 0xD0487000 ...");
    uint32_t ptr = *(volatile uint32_t *)(0xD0487000u + 0xa0u);
    uint32_t g50 = *(volatile uint32_t *)(0xD0487000u + 0x50u);
    uint32_t g54 = *(volatile uint32_t *)(0xD0487000u + 0x54u);
    uint32_t cp  = ptr & ~0x40000000u;
    static char info[160];
    snprintf(info, sizeof(info), "0xD0487000 during REC: a0=%08x 50=%08x 54=%08x\n", (unsigned)ptr, (unsigned)g50, (unsigned)g54);
    void * cap = 0; uint32_t capsz = 0;
    if (cp >= 0x01000000u && cp < 0x60000000u)
    {
        static const uint32_t sizes[] = {0x400000u, 0x200000u, 0x100000u};
        for (int s = 0; s < 3 && !cap; s++) { cap = fio_malloc(sizes[s]); if (cap) capsz = sizes[s]; }
        if (cap) memcpy(cap, (void *)UNCACHEABLE(cp), capsz);
    }
    /* wait for recording to stop so the SD is free for our write */
    int w2 = 0;
    while (RECORDING_H264 && w2 < 90000) { msleep(200); w2 += 200; }
    msleep(800);
    FILE * t = FIO_CreateFile("ML/LOGS/RC487.TXT");
    if (t) { FIO_WriteFile(t, info, strlen(info)); FIO_CloseFile(t); }
    if (cap)
    {
        FILE * f = FIO_CreateFile("ML/LOGS/RC487.BIN");
        if (f) { for (uint32_t off = 0; off < capsz; off += 0x10000u) FIO_WriteFile(f, (uint8_t *)cap + off, 0x10000); FIO_CloseFile(f); }
        fio_free(cap);
        NotifyBox(12000, "Caught 0xD0487000 %dMB -> RC487.BIN (a0=%08x)", (int)(capsz >> 20), (unsigned)ptr);
    }
    else NotifyBox(12000, "0xD0487000 a0=%08x (no buffer captured)", (unsigned)ptr);
}

/* ---- RECORDING-WINDOW SAMPLER (Debug -> "Sample raw (REC)").  The prime raw suspect idx23
 * (0xD0487000) faults when idle and can't be read safely without novel exception code, so instead we
 * sample the CONFIRMED-SAFE (non-faulting) wide imaging-WRITE channels -- idx1-8 (0xD0420000..0700,
 * the sensor front-end, UPSTREAM of the Mem1 output), idx14 (P14), idx17/18 (0xD045E000/100) -- over a
 * ~25s window while you RECORD.  Recording activates the front-end pipeline, so the live raw may appear
 * on one of these.  Per round (1/s) we read +0xa0 (safe), checksum 1KB of the pointed buffer, and log
 * channels whose content CHANGES (= live).  The first few active+changing channels are snapshotted to
 * RAM (2MB) mid-window and written AFTER you stop (no SD writes during recording).  -> RW*.BIN + RECWIN.TXT.
 * Fully boot-safe (menu task) and run-safe (no faulting reads). */
static void raw_recwin_task(void)
{
    gui_stop_menu();
    msleep(300);
    static const uint32_t ch[] = {
        0xD0420000u, 0xD0420100u, 0xD0420200u, 0xD0420300u, 0xD0420400u, 0xD0420500u, 0xD0420600u, 0xD0420700u,
        0xD0440000u, 0xD045E000u, 0xD045E100u,
    };
    const int NCH = (int)(sizeof(ch) / sizeof(ch[0]));
    static uint32_t prevchk[16];
    static void *   cap[16];
    static uint32_t capsz[16];
    for (int i = 0; i < 16; i++) { prevchk[i] = 0; cap[i] = 0; capsz[i] = 0; }
    static char lg[1024]; int n = 0;   /* shrunk to free BSS for the MMU 2-page bump (raw hook) */
    int total_cap = 0;
    n += snprintf(lg + n, sizeof(lg) - n, "REC-WINDOW sampler: SAFE wide chans sampled while recording.\n");
    NotifyBox(8000, "REC WINDOW: START RECORDING now -- sampling 25s");
    beep();
    for (int round = 0; round < 25 && n < (int)sizeof(lg) - 300; round++)
    {
        for (int i = 0; i < NCH; i++)
        {
            uint32_t a0 = *(volatile uint32_t *)(ch[i] + 0xa0u);   /* SAFE channel -- never faults */
            uint32_t cp = a0 & ~0x40000000u;
            uint32_t chk = 0;
            if (cp >= 0x01000000u && cp < 0x60000000u)
            {
                const volatile uint32_t * p = (const volatile uint32_t *)UNCACHEABLE(cp);
                for (int w = 0; w < 256; w++) chk += p[w];   /* cheap 1KB content checksum */
            }
            if (a0 && chk != prevchk[i])
            {
                n += snprintf(lg + n, sizeof(lg) - n, "r%d ch%x a0=%08x 50=%08x 54=%08x chk=%08x%s\n",
                    round, (unsigned)((ch[i] >> 8) & 0xffff), (unsigned)a0,
                    (unsigned)*(volatile uint32_t *)(ch[i] + 0x50u), (unsigned)*(volatile uint32_t *)(ch[i] + 0x54u),
                    (unsigned)chk, prevchk[i] ? " CHG" : "");
                /* snapshot active+changing channels (mid-window, after pipeline settles), max 4 */
                if (!cap[i] && round >= 3 && total_cap < 4 && cp >= 0x01000000u && cp < 0x60000000u)
                {
                    cap[i] = fio_malloc(0x200000u);
                    if (cap[i]) { capsz[i] = 0x200000u; memcpy(cap[i], (void *)UNCACHEABLE(cp & ~0x000FFFFFu), 0x200000u); total_cap++; }
                }
            }
            prevchk[i] = chk;
        }
        msleep(1000);
    }
    FILE * t = FIO_CreateFile("ML/LOGS/RECWIN.TXT");
    if (t) { FIO_WriteFile(t, lg, n); FIO_CloseFile(t); }
    int dumped = 0;
    for (int i = 0; i < NCH; i++)
    {
        if (!cap[i]) continue;
        char fn[28]; snprintf(fn, sizeof(fn), "ML/LOGS/RW%x.BIN", (unsigned)((ch[i] >> 8) & 0xffff));
        FILE * f = FIO_CreateFile(fn);
        if (f) { for (uint32_t off = 0; off < capsz[i]; off += 0x10000u) FIO_WriteFile(f, (uint8_t *)cap[i] + off, 0x10000); FIO_CloseFile(f); dumped++; }
        fio_free(cap[i]);
    }
    NotifyBox(12000, "REC window: %d chans captured -> RW*.BIN + RECWIN.TXT", dumped);
}
#endif

#ifdef FEATURE_DEBUG_PROP_DISPLAY
static CONFIG_INT("prop.i", prop_i, 0);
static CONFIG_INT("prop.j", prop_j, 0);
static CONFIG_INT("prop.k", prop_k, 0);

static MENU_UPDATE_FUNC (prop_display)
{
    unsigned prop = (prop_i << 24) | (prop_j << 16) | (prop_k);
    int* data = 0;
    size_t len = 0;
    int err = prop_get_value(prop, (void **) &data, &len);
    MENU_SET_VALUE(
    "%8x: %d: %x %x %x %x\n"
        "'%s' ",
        prop,
        len,
        len > 0x00 ? data[0] : 0,
        len > 0x04 ? data[1] : 0,
        len > 0x08 ? data[2] : 0,
        len > 0x0c ? data[3] : 0,
        strlen((const char *) data) < 100 ? (const char *) data : ""
    );
}

void prop_dump()
{
    FILE* f = FIO_CreateFile("ML/LOGS/PROP.LOG");
    if (!f)
    {
        return;
    }

    FILE* g = FIO_CreateFile("ML/LOGS/PROP-STR.LOG");
    if (!g)
    {
        FIO_CloseFile(f);
        return;
    }

    unsigned i, j, k;

    for( i=0 ; i<256 ; i++ )
    {
        if (i > 0x10 && i != 0x80) continue;
        for( j=0 ; j<=0xA ; j++ )
        {
            for( k=0 ; k<0x50 ; k++ )
            {
                unsigned prop = 0
                    | (i << 24)
                    | (j << 16)
                    | (k <<  0);

                bmp_printf(FONT_LARGE, 0, 0, "PROP %x...", prop);
                int* data = 0;
                size_t len = 0;
                int err = prop_get_value(prop, (void **) &data, &len);
                if (!err)
                {
                    my_fprintf(f, "\nPROP %8x: %5d:", prop, len );
                    my_fprintf(g, "\nPROP %8x: %5d:", prop, len );
                    for (unsigned int i = 0; i < (MIN(len,40)+3)/4; i++)
                    {
                        my_fprintf(f, "%8x ", data[i]);
                    }
                    if (strlen((const char *) data) < 100) my_fprintf(g, "'%s'", data);
                }
            }
        }
    }
    FIO_CloseFile(f);
    FIO_CloseFile(g);
    beep();
    redraw();
}

static void prop_toggle_i(void* priv, int unused) {prop_i = prop_i < 5 ? prop_i + 1 : prop_i == 5 ? 0xE : prop_i == 0xE ? 0x80 : 0; }
static void prop_toggle_j(void* priv, int unused) {prop_j = MOD(prop_j + 1, 0x10); }
static void prop_toggle_k(void* priv, int dir) {if (dir < 0) prop_toggle_j(priv, dir); prop_k = MOD(prop_k + 1, 0x51); }
#endif

#ifdef CONFIG_KILL_FLICKER
void menu_kill_flicker()
{
    gui_stop_menu();
    canon_gui_disable_front_buffer();
}
#endif


extern MENU_UPDATE_FUNC(tasks_print);
extern MENU_UPDATE_FUNC(batt_display);
extern MENU_SELECT_FUNC(tasks_toggle_flags);

extern int show_cpu_usage_flag;

static int gui_events_show = 0;
static MENU_SELECT_FUNC(gui_events_toggle);

static struct menu_entry debug_menus[] = {
    MENU_PLACEHOLDER("File Manager"),
#ifdef CONFIG_HEXDUMP
    {
        .name = "Memory Browser",
        .priv = &hexdump_enabled,
        .max = 1,
        .help = "Display memory contents in real-time (hexdump).",
        .children =  (struct menu_entry[]) {
            {
                .name = "HexDump",
                .priv = &hexdump_addr,
                .max = 0x20000000,
                .unit = UNIT_HEX,
                .icon_type = IT_PERCENT,
                .help = "Address to be analyzed. Press Q to select the digit to edit."
            },
            {
                .name = "Pointer dereference",
                .select = hexdump_deref,
                .help = "Changes address to *(int*)addr [SET] or goes back [PLAY]."
            },
            {
                .name = "Val hex32",
                .update = hexdump_print_value_hex,
                .select = hexdump_toggle_value_int32,
                .help = "Value as hex."
            },
            {
                .name = "Val int32",
                .update = hexdump_print_value_int32,
                .select = hexdump_toggle_value_int32,
                .help = "Value as int32."
            },
            {
                .name = "Val int16",
                .update = hexdump_print_value_int16,
                .select = hexdump_toggle_value_int16,
                .help = "Value as 2 x int16. Toggle: changes second value."
            },
            {
                .name = "Val int8",
                .update = hexdump_print_value_int8,
                .help = "Value as 4 x int8."
            },
            {
                .name = "Val string",
                .update = hexdump_print_value_str,
                .help = "Value as string."
            },
            MENU_EOL
        },
    },
#endif
    /*{
        .name        = "Flashlight",
        .select        = flashlight_lcd,
        .select_reverse = flashlight_frontled,
        .help = "Turn on the front LED [PLAY] or make display bright [SET]."
    },*/
    #ifdef FEATURE_SCREENSHOT
    {
        .name   = "Screenshot - 10s",
        .select = screenshot_start,
        .help   = "Screenshot after 10 seconds => VRAMx.BMP.",
        .help2  = "The screenshot will contain BMP and YUV overlays."
    },
    #endif
/*    {
        .name = "Menu screenshots",
        .select     = (void (*)(void*,int))run_in_separate_task,
        .priv = screenshots_for_menu,
        .help = "Take a screenshot for each ML menu.",
    }, */
#if CONFIG_DEBUGMSG
    #if 0
    {
        .name = "Draw palette",
        .select        = bmp_draw_palette,
        .help = "Display a test pattern to see the color palette."
    },
    #endif
    {
        .name = "Spy properties",
        .priv = &draw_prop,
        .max = 1,
        .help = "Show properties as they change."
    },
/*    {
        .name        = "Dialog test",
        .select        = dlg_test,
        .help = "Dialog templates (up/dn) and color palettes (left/right)"
    },*/
#endif
    {
        .name        = "Dump ROM and RAM",
        .priv        = dump_rom_task,
        .select      = run_in_separate_task,
    #if defined(CONFIG_DIGIC_45)
        .help = "ROM0.BIN:F0000000, ROM1.BIN:F8000000, RAM4.BIN"
    #elif defined(CONFIG_DIGIC_6)
        .help = "ROM0.BIN:      NA, ROM1.BIN:FE000000, RAM4.BIN"
    #elif defined(CONFIG_DIGIC_78X)
        .help = "ROM0.BIN:E0000000, ROM1.BIN:F0000000, RAM4.BIN"
    #endif
    },
    {
        .name        = "Dump image buffers",
        .priv        = dump_img_task,
        .select      = run_in_separate_task,
        .help = "Dump all image buffers (LV, HD, RAW) from current video mode."
    },
#ifdef FEATURE_UNMOUNT_SD_CARD
    {
        .name        = "Unmount SD card",
        .priv        = unmount_sd_card,
        .select      = run_in_separate_task,
        .help        = "Run before uploading files to a Wi-Fi card, to avoid data corruption.",
        .help2       = "No further writes will be performed on your card from the camera.",
    },
#endif
#ifdef FEATURE_DONT_CLICK_ME
    {
        .name        = "Don't click me!",
        .priv =         run_test,
        .select        = run_in_separate_task,
        .help = "The camera may turn into a 1DX or it may explode."
    },
#endif
#ifdef FEATURE_BOOTFLAG_MENU
    {
        .name       = "Bootflag settings",
        .select     = menu_open_submenu,
        .help       = "Change camera bootflag status",
        .children =  (struct menu_entry[]) {
            {
                .name   = "Disable bootflag",
                .select = bootflag_disable,
                .help   = "Calls DisableBootDisk EvProc"
            },
            {
                .name   = "Enable bootflag",
                .select = bootflag_enable,
                .help   = "Calls EnableBootDisk EvProc"
            },
            MENU_EOL,
        },
    },
#endif
#ifdef CONFIG_DEBUG_INTERCEPT
    {
        .name        = "DM Log",
        .priv        = j_debug_intercept,
        .select      = run_in_separate_task,
        .help = "Log DebugMessages"
    },
    {
        .name        = "TryPostEvent Log",
        .priv        = j_tp_intercept,
        .select      = run_in_separate_task,
        .help = "Log TryPostEvents"
    },
#endif
#ifdef FEATURE_SHOW_TASKS
    {
        .name = "Show tasks",
        .select = menu_open_submenu,
        .help = "Displays the tasks started by Canon and Magic Lantern.",
        .children =  (struct menu_entry[]) {
            {
                .name = "Task list",
                .update = tasks_print,
                .select = tasks_toggle_flags,
                #ifdef CONFIG_VXWORKS
                .help = "Task info: name, priority, stack memory usage.",
                #else
                .help = "Task info: ID, name, priority, wait_id, mem, state.",
                #endif
            },
            MENU_EOL
        }
    },
#endif
#ifdef FEATURE_SHOW_CPU_USAGE
#ifdef CONFIG_TSKMON
    {
        .name = "Show CPU usage",
        .priv = &show_cpu_usage_flag,
        .max = 3,
        .choices = (const char *[]) {"OFF", "Percentage", "Busy tasks (ABS)", "Busy tasks (REL)"},
        .help = "Display total CPU usage (percentage).",
    },
#endif
#endif
#ifdef FEATURE_SHOW_GUI_EVENTS
    {
        .name   = "Show GUI events",
        .priv   = &gui_events_show,
        .select = gui_events_toggle,
        .max    = 1,
        .help   = "Display GUI events (button codes).",
    },
#endif
#ifdef FEATURE_GUIMODE_TEST
    {
        .name = "Test GUI modes (DANGEROUS!!!)",
        .select = run_in_separate_task,
        .priv = guimode_test,
        .help = "Cycle through all GUI modes and take screenshots.",
    },
#endif
#ifdef CONFIG_R
    {
        .name        = "Brightness probe",
        .priv        = brightness_probe_task,
        .select      = run_in_separate_task,
        .help  = "LIVEVIEW + vary light (~2.5min, passive): logs PROP_LV_BV vs the scene.",
        .help2 = "Finds a metering signal for adaptive-exposure timelapse. -> BRIGHT.TXT.",
    },
    {
        .name        = "Arm raw hook",
        .priv        = rawhk_task,
        .select      = run_in_separate_task,
        .help  = "Select, then press REC + RECORD 12s: hooks FUN_e05364b6 (the +0xa0 buffer setter).",
        .help2 = "Logs which chan gets the raw buffer (idx+a0). Auto-unpatch. -> ML/LOGS/RAWHK.TXT.",
    },
    {
        .name        = "Slurp raw",
        .priv        = slurp_raw_task,
        .select      = run_in_separate_task,
        .help  = "Select, then press REC + RECORD ~10s: slurp idx7<-sensor-raw(conn0) into our buffer.",
        .help2 = "Experimental mlv_lite-style raw capture. -> ML/LOGS/SLURP.BIN (+SLURP.TXT regs).",
    },
    {
        .name        = "Raw bright test",
        .priv        = raw_bright_task,
        .select      = run_in_separate_task,
        .help  = "IN LIVEVIEW: multi-offset scan of safe channels for the LIVE buffer. HOLD HALF-PRESS.",
        .help2 = "Logs frame-changing buffers (CHG) + content sample. -> ML/LOGS/RAWBR.TXT.",
    },
    {
        .name        = "Dump raw buf",
        .priv        = raw_dump_task,
        .select      = run_in_separate_task,
        .help  = "IN LIVEVIEW (hold half-press): dump 8MB of the live D0420700+0xa0 buffer.",
        .help2 = "For PC render to confirm a moving image. -> ML/LOGS/RAW.BIN (+ RAW.TXT).",
    },
    {
        .name        = "Dump raw cands",
        .priv        = raw_cand_task,
        .select      = run_in_separate_task,
        .help  = "IN MOVIE LIVEVIEW: dump the WIDE imaging-write channels (idx17/18/23/24/26-28).",
        .help2 = "Hunts the real Mem1/HeadToRaw channel. -> ML/LOGS/RC*.BIN + RAWCAND.TXT (crash-bisect LASTCH.TXT).",
    },
    {
        .name        = "Catch raw (REC)",
        .priv        = raw_catch_task,
        .select      = run_in_separate_task,
        .help  = "ARM, then press REC: catches the raw channel 0xD0487000 while it's powered.",
        .help2 = "Captures buffer to RAM during recording, writes RC487.BIN after stop (+ RC487.TXT).",
    },
    {
        .name        = "Sample raw (REC)",
        .priv        = raw_recwin_task,
        .select      = run_in_separate_task,
        .help  = "SELECT then RECORD ~25s: samples the SAFE wide channels (idx1-8/14/17/18).",
        .help2 = "Safe (no faulting reads). Logs live channels -> RECWIN.TXT, snapshots -> RW*.BIN.",
    },
#endif
    MENU_PLACEHOLDER("Free Memory"),
#ifdef FEATURE_SHOW_IMAGE_BUFFERS_INFO
    {
        .name = "Image buffers",
        .update = image_buf_display,
        .icon_type = IT_ALWAYS_ON,
        .help = "Display the image buffer sizes (LiveView and Craw).",
        //.essential = 0,
    },
#endif
#ifdef FEATURE_SHOW_SHUTTER_COUNT
    {
        .name = "Shutter Count",
        .update = shuttercount_display,
        //.essential = FOR_MOVIE | FOR_PHOTO,
        #if defined(CONFIG_DIGIC_8X)
        .help = "Number of shutter actions. Open submenu to learn more.",
        .select = menu_open_submenu,
        .submenu_width = 710,
        .children =  (struct menu_entry[]) {
            {
                .name = "Total Shutter",
                .update = totalshutter_display,
                .icon_type = IT_ALWAYS_ON,
                .help = "Number of mechanical shutter actions (incl. sensor cleaning)",
            },
            {
                .name = "Total Mirror",
                .update = totalmirror_display,
                .icon_type = IT_ALWAYS_ON,
                .help = "Number of mirror move actions (DSLR, 0 on mirrorless)",
            },
            {
                .name = "Total Shots",
                .update = totalshoot_display,
                .icon_type = IT_ALWAYS_ON,
                .help = "Number of photos made. Incl. silent (electronic) shots",
            },
            MENU_EOL,
        },
        #else // no submenu, classic style
        .help = "Number of pics taken + number of LiveView actuations",
        .icon_type = IT_ALWAYS_ON,
        #endif
    },
#endif

#ifdef FEATURE_SHOW_CMOS_TEMPERATURE
    {
        .name = "Internal Temp",
        .update = efictemp_display,
        .icon_type = IT_ALWAYS_ON,
	 #ifdef EFIC_CELSIUS
        .help = "EFIC chip temperature (somewhere on the mainboard).",
	 #else
	.help = "EFIC chip temperature (raw values).",
	.help2 = "http://www.magiclantern.fm/forum/index.php?topic=9673.0",
	 #endif
        //.essential = FOR_MOVIE | FOR_PHOTO,
    },
#endif
    #if 0 // CONFIG_5D2
    {
        .name = "Ambient light",
        //~.display = ambient_display,
        .help = "Ambient light from the sensor under LCD, in raw units.",
        //.essential = FOR_MOVIE | FOR_PHOTO,
    },
    #endif
#ifdef CONFIG_BATTERY_INFO
    {
        .name = "Battery level",
        .update = batt_display,
        .help = "Battery remaining. Wait for 2% discharge before reading.",
        .icon_type = IT_ALWAYS_ON,
    },
#endif
#ifdef FEATURE_DEBUG_PROP_DISPLAY
    {
        .name = "PROP Display",
        .update = prop_display,
        .select = prop_toggle_k,
        // .select_reverse = prop_toggle_j,
        .select_Q = prop_toggle_i,
        .help = "Raw property display (read-only)",
    },
#endif
};

#if CONFIG_DEBUGMSG

static void * debug_token;

static void
debug_token_handler(
    void *            token,
    void *            arg1,
    void *            arg2,
    void *            arg3
)
{
    debug_token = token;
    DebugMsg( DM_MAGIC, 3, "token %08x arg=%08x %08x %08x",
        (unsigned) token,
        (unsigned) arg1,
        (unsigned) arg2,
        (unsigned) arg3
    );
}

//~ static int dbg_propn = 0;
#define MAXPROP 30
static unsigned dbg_props[MAXPROP] = {0};
static unsigned dbg_props_len[MAXPROP] = {0};
static unsigned dbg_props_a[MAXPROP] = {0};
static unsigned dbg_props_b[MAXPROP] = {0};
static unsigned dbg_props_c[MAXPROP] = {0};
static unsigned dbg_props_d[MAXPROP] = {0};
static unsigned dbg_props_e[MAXPROP] = {0};
static unsigned dbg_props_f[MAXPROP] = {0};
static void dbg_draw_props(int changed)
{
    dbg_last_changed_propindex = changed;
    int i;
    for (i = 0; i < dbg_propn; i++)
    {
    	int x =  80;
        unsigned property = dbg_props[i];
        unsigned len = dbg_props_len[i];
#ifdef CONFIG_VXWORKS
        uint32_t fnt = FONT_MONO_20;
        unsigned y =  15 + i * 20;
#else
        uint32_t fnt = FONT_MONO_12;
        int y =  15 + i * 12;
#endif
        if (i == changed) fnt = FONT(fnt, 5, COLOR_BG);
        char msg[100];
        snprintf(msg, sizeof(msg),
#ifdef CONFIG_VXWORKS
            "%08x %04x: %8lx %8lx %8lx %8lx",
#else
            "%08x %04x: %8lx %8lx %8lx %8lx %8lx %8lx",
#endif
            property,
            len,
            len > 0x00 ? dbg_props_a[i] : 0,
            len > 0x04 ? dbg_props_b[i] : 0,
            len > 0x08 ? dbg_props_c[i] : 0,
            len > 0x0c ? dbg_props_d[i] : 0
            #ifndef CONFIG_VXWORKS
           ,len > 0x10 ? dbg_props_e[i] : 0,
            len > 0x14 ? dbg_props_f[i] : 0
            #endif
        );
        bmp_puts(fnt, &x, &y, msg);
    }
}


static void *
debug_property_handler(
    unsigned        property,
    void *            UNUSED_ATTR( priv ),
    void *            buf,
    unsigned        len
)
{
    const uint32_t * const addr = buf;

    /*printf("Prop %08x: %2x: %08x %08x %08x %08x\n",
        property,
        len,
        len > 0x00 ? addr[0] : 0,
        len > 0x04 ? addr[1] : 0,
        len > 0x08 ? addr[2] : 0,
        len > 0x0c ? addr[3] : 0
    );*/

    if( !draw_prop )
        goto ack;

    // maybe the property is already in the array
    int i;
    for (i = 0; i < dbg_propn; i++)
    {
        if (dbg_props[i] == property)
        {
            dbg_props_len[i] = len;
            dbg_props_a[i] = addr[0];
            dbg_props_b[i] = addr[1];
            dbg_props_c[i] = addr[2];
            dbg_props_d[i] = addr[3];
            dbg_props_e[i] = addr[4];
            dbg_props_f[i] = addr[5];
            dbg_draw_props(i);
            goto ack; // return with cleanup
        }
    }
    // new property
    if (dbg_propn >= MAXPROP) dbg_propn = MAXPROP-1; // too much is bad :)
    dbg_props[dbg_propn] = property;
    dbg_props_len[dbg_propn] = len;
    dbg_props_a[dbg_propn] = addr[0];
    dbg_props_b[dbg_propn] = addr[1];
    dbg_props_c[dbg_propn] = addr[2];
    dbg_props_d[dbg_propn] = addr[3];
    dbg_props_e[dbg_propn] = addr[4];
    dbg_props_f[dbg_propn] = addr[5];
    dbg_propn++;
    dbg_draw_props(dbg_propn);

ack:
    return (void*)_prop_cleanup( debug_token, property );
}

#endif

#if defined(CONFIG_500D)
#define num_properties 2048
#elif defined(CONFIG_5DC)
#define num_properties 202
#else
#define num_properties 8192
#endif

void
debug_init( void )
{
#if CONFIG_DEBUGMSG
    draw_prop = 0;
    static unsigned* property_list = 0;
    if (!property_list) property_list = malloc(num_properties * sizeof(unsigned));
    if (!property_list) return;
    unsigned i, j, k;
    unsigned actual_num_properties = 0;

    unsigned is[] = {0x2, 0x80, 0xe, 0x5, 0x4, 0x1, 0x0};
    for( i=0 ; i<COUNT(is) ; i++ )
    {
        for( j=0 ; j<=0xA ; j++ )
        {
            for( k=0 ; k<0x50 ; k++ )
            {
                unsigned prop = 0
                    | (is[i] << 24)
                    | (j << 16)
                    | (k <<  0);

                property_list[ actual_num_properties++ ] = prop;

                if( actual_num_properties >= num_properties )
                    goto thats_all;
            }
        }
    }

thats_all:
    prop_register_slave(
        property_list,
        actual_num_properties,
        debug_property_handler,
        0,
        0
    );
#endif

}

CONFIG_INT( "debug.timed-dump",        timed_dump, 0 );

//~ CONFIG_INT( "debug.dump_prop", dump_prop, 0 );
//~ CONFIG_INT( "debug.dumpaddr", dump_addr, 0 );
//~ CONFIG_INT( "debug.dumplen", dump_len, 0 );

/*
struct bmp_file_t * logo = (void*) -1;
void load_logo()
{
    if (logo == (void*) -1)
        logo = bmp_load("ML/DOC/logo.bmp",0);
}
void show_logo()
{
    load_logo();
    if ((int)logo > 0)
    {
        kill_flicker(); msleep(100);
        bmp_draw_scaled_ex(logo, 360 - logo->width/2, 240 - logo->height/2, logo->width, logo->height, 0, 0);
    }
}*/

// initialization done AFTER reading the config file,
// but BEFORE starting ML tasks
void
debug_init_stuff( void )
{
    //~ set_pic_quality(PICQ_RAW);

    #ifdef CONFIG_WB_WORKAROUND
    if (is_movie_mode())
    {
        extern void restore_kelvin_wb(); /* movtweaks.c */
        restore_kelvin_wb();
    }
    #endif

    #ifdef CONFIG_5D3
    _card_tweaks();
    #endif
}

TASK_CREATE( "debug_task", debug_loop_task, 0, 0x1e, 0x2000 );


#ifdef CONFIG_INTERMEDIATE_ISO_INTERCEPT_SCROLLWHEEL
    #ifndef FEATURE_EXPO_ISO
    #error This requires FEATURE_EXPO_ISO.
    #endif

int iso_intercept = 1;

void iso_adj(int prev_iso, int sign)
{
    if (sign)
    {
        lens_info.raw_iso = prev_iso;
        iso_intercept = 0;
        iso_toggle(0, sign);
        if (lens_info.iso > 6400) lens_set_rawiso(0);
        iso_intercept = 1;
    }
}

int iso_adj_flag = 0;
int iso_adj_old = 0;
int iso_adj_sign = 0;

void iso_adj_task(void* unused)
{
    TASK_LOOP
    {
        msleep(20);
        if (iso_adj_flag)
        {
            iso_adj_flag = 0;
            iso_adj(iso_adj_old, iso_adj_sign);
            lens_display_set_dirty();
        }
    }
}

TASK_CREATE("iso_adj_task", iso_adj_task, 0, 0x1a, 0);

PROP_HANDLER(PROP_ISO)
{
    static unsigned int prev_iso = 0;
    if (!prev_iso) prev_iso = lens_info.raw_iso;

    if (iso_intercept && ISO_ADJUSTMENT_ACTIVE && lv && lv_disp_mode == 0 && is_movie_mode())
    {
        if ((prev_iso && buf[0] && prev_iso < buf[0]) || // 100 -> 200 => +
            (prev_iso >= 112 && buf[0] == 0)) // 3200+ -> auto => +
        {
            //~ bmp_printf(FONT_LARGE, 50, 50, "[%d] ISO+", k++);
            iso_adj_old = prev_iso;
            iso_adj_sign = 1;
            iso_adj_flag = 1;
        }
        else if ((prev_iso && buf[0] && prev_iso > buf[0]) || // 200 -> 100 => -
            (prev_iso <= 88 && buf[0] == 0)) // 400- -> auto => -
        {
            //~ bmp_printf(FONT_LARGE, 50, 50, "[%d] ISO-", k++);
            iso_adj_old = prev_iso;
            iso_adj_sign = -1;
            iso_adj_flag = 1;
        }
    }
    prev_iso = buf[0];
}

#endif

#ifdef CONFIG_RESTORE_AFTER_FORMAT

static int keep_ml_after_format = 1;

static void HijackFormatDialogBox()
{
    if (MEM(DIALOG_MnCardFormatBegin) == 0) return;
    struct gui_task * current = gui_task_list.current;
    struct dialog * dialog = current->priv;
    if (dialog && !streq(dialog->type, "DIALOG")) return;

    if (keep_ml_after_format)
        dialog_set_property_str(dialog, 4, "Format card, keep ML " FORMAT_BTN_NAME);
    else
        dialog_set_property_str(dialog, 4, "Format card, remove ML " FORMAT_BTN_NAME);
    dialog_redraw(dialog);
}

static void HijackCurrentDialogBox(int string_id, char* msg)
{
    struct gui_task * current = gui_task_list.current;
    struct dialog * dialog = current->priv;
    if (dialog && !streq(dialog->type, "DIALOG")) return;
    dialog_set_property_str(dialog, string_id, msg);
    dialog_redraw(dialog);
}

int handle_keep_ml_after_format_toggle(struct event * event)
{
    if (event->param == FORMAT_BTN && MENU_MODE && MEM(DIALOG_MnCardFormatBegin))
    {
        keep_ml_after_format = !keep_ml_after_format;
        fake_simple_button(MLEV_HIJACK_FORMAT_DIALOG_BOX);
        return 0;
    }
    
    return 1;
}

/**
 * for testing dialogs and string IDs
 */

static void HijackDialogBox()
{
    struct gui_task * current = gui_task_list.current;
    struct dialog * dialog = current->priv;
    if (dialog && !streq(dialog->type, "DIALOG")) return;
    int i;
    for (i = 0; i<255; i++) {
            char s[30];
            snprintf(s, sizeof(s), "%d", i);
            dialog_set_property_str(dialog, i, s);
    }
    dialog_redraw(dialog);
}

struct tmp_file {
    char name[50];
    void* buf;
    int size;
    int sig;
};

static struct tmp_file * tmp_files = 0;
static int tmp_file_index = 0;
static void* tmp_buffer = 0;
static void* tmp_buffer_ptr = 0;
#define TMP_MAX_BUF_SIZE 15000000

static int TmpMem_Init()
{
    ASSERT(!tmp_buffer);
    ASSERT(!tmp_files);
    static int retries = 0;
    tmp_file_index = 0;
    if (!tmp_files) tmp_files = malloc(200 * sizeof(struct tmp_file));
    if (!tmp_files)
    {
        retries++;
        HijackCurrentDialogBox(4,
            retries > 2 ? "Restart your camera (malloc error)." :
                          "Format: malloc error :("
            );
        beep();
        msleep(2000);
        return 0;
    }

    if (!tmp_buffer) tmp_buffer = (void*)fio_malloc(TMP_MAX_BUF_SIZE);
    if (!tmp_buffer)
    {
        retries++;
        HijackCurrentDialogBox(4,
            retries > 2 ? "Restart your camera (fio_malloc err)." :
                          "Format: fio_malloc error, retrying..."
        );
        beep();
        msleep(2000);
        free(tmp_files); tmp_files = 0;
        return 0;
    }

    retries = 0;
    tmp_buffer_ptr = tmp_buffer;

    return 1;
}

static void TmpMem_Done()
{
    free(tmp_files); tmp_files = 0;
    fio_free(tmp_buffer); tmp_buffer = 0;
}

static void TmpMem_UpdateSizeDisplay(int counting)
{
    int size = tmp_buffer_ptr - tmp_buffer;
    int size_mb = size * 10 / 1024 / 1024;

    char msg[100];
    snprintf(msg, sizeof(msg), "Format       (ML size: %s%d.%d MB%s)", counting ? "> " : "", size_mb/10, size_mb%10, counting ? "..." : "");
    HijackCurrentDialogBox(3, msg);
}

static void TmpMem_AddFile(char* filename)
{
    if (!tmp_buffer) return;
    if (!tmp_buffer_ptr) return;

    int filesize = FIO_GetFileSize_direct(filename);
    if (filesize == -1) return;
    if (tmp_file_index >= 200) return;
    if (tmp_buffer_ptr + filesize + 10 >= tmp_buffer + TMP_MAX_BUF_SIZE) return;
    
    /* don't add the same file twice */
    for (int i = 0; i < tmp_file_index; i++)
        if (streq(tmp_files[i].name, filename))
            return;

    read_file(filename, tmp_buffer_ptr, filesize);
    snprintf(tmp_files[tmp_file_index].name, 50, "%s", filename);
    tmp_files[tmp_file_index].buf = tmp_buffer_ptr;
    tmp_files[tmp_file_index].size = filesize;
    tmp_files[tmp_file_index].sig = compute_signature(tmp_buffer_ptr, filesize/4);
    tmp_file_index++;
    tmp_buffer_ptr += ALIGN32SUP(filesize);

    /* no not update on every file, else it takes too long (90% of time updating display) */
    static int aux = 0;
    if(should_run_polling_action(500, &aux))
    {
        char msg[100];

        snprintf(msg, sizeof(msg), "Reading %s...", filename, tmp_buffer_ptr);
        HijackCurrentDialogBox(4, msg);
        TmpMem_UpdateSizeDisplay(1);
    }
}

static void CopyMLDirectoryToRAM_BeforeFormat(char* dir, int (*is_valid_filename)(char*), int recursive_levels)
{
    struct fio_file file;
    struct fio_dirent * dirent = FIO_FindFirstEx( dir, &file );
    if( IS_ERROR(dirent) )
        return;

    do {
        if (file.name[0] == '.' || file.name[0] == '_') continue;
        if (file.mode & ATTR_DIRECTORY)
        {
            if (recursive_levels > 0)
            {
                char new_dir[0x80];
                snprintf(new_dir, sizeof(new_dir), "%s%s/", dir, file.name);
                CopyMLDirectoryToRAM_BeforeFormat(new_dir, is_valid_filename, recursive_levels-1);
            }
            continue; // is a directory
        }
        
        if (is_valid_filename && !is_valid_filename(file.name))
        {
            continue;
        }

        char fn[0x80];
        snprintf(fn, sizeof(fn), "%s%s", dir, file.name);
        TmpMem_AddFile(fn);

    } while( FIO_FindNextEx( dirent, &file ) == 0);
    FIO_FindClose(dirent);
}

static int is_valid_fir_filename(char* filename)
{
    int n = strlen(filename);
    if ((n > 4) && (streq(filename + n - 4, ".FIR") || streq(filename + n - 4, ".fir")))
        return 1;
    return 0;
}

static int is_valid_log_filename(char* filename)
{
    int n = strlen(filename);
    if ((n > 4) && (streq(filename + n - 4, ".LOG") || streq(filename + n - 4, ".log")))
        return 1;
    return 0;
}

static void CopyMLFilesToRAM_BeforeFormat()
{
    /* this is the most important file, read it first */
    TmpMem_AddFile("AUTOEXEC.BIN");
    
    /* some important subdirectories from ML/ */
    CopyMLDirectoryToRAM_BeforeFormat("ML/FONTS/", 0, 0);
    CopyMLDirectoryToRAM_BeforeFormat("ML/MODULES/", 0, 0);
    CopyMLDirectoryToRAM_BeforeFormat("ML/SETTINGS/", 0, 1);

    /* FIR files from root dir */
    CopyMLDirectoryToRAM_BeforeFormat("", is_valid_fir_filename, 0);
    
    /* everything else from ML dir */
    CopyMLDirectoryToRAM_BeforeFormat("ML/", 0, 2);
    
    /* and, if we still have free space, also keep the LOG files from root dir */
    CopyMLDirectoryToRAM_BeforeFormat("", is_valid_log_filename, 0);
    
    /* restore Toshiba FlashAir files, if any */
    /* (normally, formatting this card from camera disables wifi operation) */
    /* (not sure which of those are strictly needed) */
    CopyMLDirectoryToRAM_BeforeFormat("B:/SD_WLAN/", 0, 0);
    CopyMLDirectoryToRAM_BeforeFormat("B:/GUPIXINF/", 0, 1);
    TmpMem_AddFile("B:/DCIM/100__TSB/FA000001.JPG");

    TmpMem_UpdateSizeDisplay(0);
}

// check if autoexec.bin is present on the card
static int check_autoexec()
{
    return is_file("AUTOEXEC.BIN");
}

static void CopyMLFilesBack_AfterFormat()
{
    int i;
    char msg[100];
    int aux = 0;
    for (i = 0; i < tmp_file_index; i++)
    {
        if(should_run_polling_action(500, &aux))
        {
            snprintf(msg, sizeof(msg), "Restoring %s...", tmp_files[i].name);
            HijackCurrentDialogBox(FORMAT_STR_LOC, msg);
        }
        save_mem_to_file(tmp_files[i].buf, tmp_files[i].size, tmp_files[i].name);
        int sig = compute_signature(tmp_files[i].buf, tmp_files[i].size/4);
        if (sig != tmp_files[i].sig)
        {
            snprintf(msg, sizeof(msg), "Could not restore %s :(", tmp_files[i].name);
            HijackCurrentDialogBox(FORMAT_STR_LOC, msg);
            msleep(2000);
            FIO_RemoveFile(tmp_files[i].name);
            if (i <= 1) return;
            //else: if it copies AUTOEXEC.BIN and fonts, ignore the error, it's safe to run
        }
    }

    /* make sure we don't enable bootflag when there is no autoexec.bin (anymore) */
    if(check_autoexec())
    {
        HijackCurrentDialogBox(FORMAT_STR_LOC, "Writing bootflags...");
        
        extern int bootflag_write_bootblock(void);
        if (!bootflag_write_bootblock())
        {
            beep_times(3);
            NotifyBox(5000, "Bootflags not written, use EosCard");
        }
    }

    HijackCurrentDialogBox(FORMAT_STR_LOC, "Magic Lantern restored :)");
    msleep(2000);
}

static void restart_after_format()
{
    /* restart the camera after formatting */
    HijackCurrentDialogBox(FORMAT_STR_LOC, "Restarting camera...");
    msleep(1000);
    
    int reboot = 0;
    prop_request_change(PROP_REBOOT, &reboot, 4);
}

static void HijackFormatDialogBox_main()
{
    if (!MENU_MODE) return;
    if (MEM(DIALOG_MnCardFormatBegin) == 0) return;
    // at this point, Format dialog box is active
    
    #ifdef CONFIG_DUAL_SLOT
    int ml_on_cf = (get_ml_card()->drive_letter[0] == 'A');
    if (ml_on_cf != FORMATTING_CF_CARD)
    {
        /* we are not formatting the ML card, no need to restore anything */
        return;
    }
    #endif

    // make sure we have something to restore :)
    if (!check_autoexec()) return;

    gui_uilock(UILOCK_EVERYTHING);
    
    while (!TmpMem_Init())  /* may fail because of not enough memory */
        msleep(100);

    // before user attempts to do something, copy ML files to RAM
    CopyMLFilesToRAM_BeforeFormat();
    gui_uilock(UILOCK_NONE);

    // all files copied, we can change the message in the format box and let the user know what's going on
    fake_simple_button(MLEV_HIJACK_FORMAT_DIALOG_BOX);

    // waiting to exit the format dialog somehow
    while (MEM(DIALOG_MnCardFormatBegin))
        msleep(200);

    // and maybe to finish formatting the card
    while (MEM(DIALOG_MnCardFormatExecute))
        msleep(50);

    // card was formatted (autoexec no longer there) => restore ML
    if (keep_ml_after_format && !check_autoexec())
    {
        gui_uilock(UILOCK_EVERYTHING);
        CopyMLFilesBack_AfterFormat();
        TmpMem_Done();
        restart_after_format();
        /* needed? */
        gui_uilock(UILOCK_NONE);
    }
    else
    {
        TmpMem_Done();
    }
}
#endif

void debug_menu_init()
{
    #ifdef FEATURE_LV_DISPLAY_PRESETS
    extern struct menu_entry livev_cfg_menus[];
    menu_add( "Prefs", livev_cfg_menus,  1);
    #endif

    crop_factor_menu_init();
    customize_menu_init();
    menu_add( "Debug", debug_menus, COUNT(debug_menus) );
    
    #ifdef FEATURE_SHOW_FREE_MEMORY
    mem_menu_init();
    #endif
    
    movie_tweak_menu_init();
}

static MENU_SELECT_FUNC(gui_events_toggle)
{
    gui_events_show = !gui_events_show;

    if (gui_events_show) {
        console_show();
    } else {
        console_hide();
    }
}

void spy_event(struct event *event)
{
    if (!gui_events_show)
        return;

    if (event == NULL)
    {
        printf("Event NULL\n");
    }
    else
    {
        printf("Event param: %8x arg: %8x ", event->param, event->arg);
        if (event->obj == NULL)
        {
            printf(" obj:     NULL\n");
        }
        else
        {
            if ((int)event->obj & 0xf0000000)
            { // Old code avoids deref of these pointers.
              // There is no comment to explain why.  Possibly because if
              // they're an address, it would be in ROM?
                printf(" obj: %8x\n", event->obj);
            }
            else
            { // normal DryOS event
                // Old cams expect event->obj to be a valid pointer, but
                // it is not always on new cams.  Possibly, it wasn't always
                // on old cams...  but they don't crash on pointer derefs
                // to low memory locations.

                if ((int)event->obj < 0x4000)
                { // unpleasant hack, assume these are not valid pointers
                    printf(" obj: %8x\n", event->obj);
                }
                else
                {
                    // FIXME SJE work out what these fields are,
                    // at least enough to make them part of the event
                    // struct and stop doing dirty offsets reads through
                    // event->obj directly.
                    printf("*obj: %8x, %8x, %8x\n",
                           *(int*)(event->obj),
                           *(int*)(event->obj + 4),
                           *(int*)(event->obj + 8));
                }
            }
        }
    }
}

#ifdef CONFIG_5DC
static int halfshutter_pressed;
bool get_halfshutter_pressed() { return halfshutter_pressed; }
#else
bool get_halfshutter_pressed() { return HALFSHUTTER_PRESSED && !dofpreview; }
#endif

static int zoom_in_pressed = 0;
static int zoom_out_pressed = 0;
int get_zoom_out_pressed() { return zoom_out_pressed; }

int handle_buttons_being_held(struct event * event)
{
    // keep track of buttons being pressed
    #ifdef CONFIG_5DC
    if (event->param == BGMT_PRESS_HALFSHUTTER) halfshutter_pressed = 1;
    if (event->param == BGMT_UNPRESS_HALFSHUTTER) halfshutter_pressed = 0;
    #endif
    #ifdef BGMT_UNPRESS_ZOOM_IN
    if (event->param == BGMT_PRESS_ZOOM_IN) {zoom_in_pressed = 1; zoom_out_pressed = 0; }
    if (event->param == BGMT_UNPRESS_ZOOM_IN) {zoom_in_pressed = 0; zoom_out_pressed = 0; }
    #endif
    #ifdef BGMT_PRESS_ZOOM_OUT
    if (event->param == BGMT_PRESS_ZOOM_OUT) { zoom_out_pressed = 1; zoom_in_pressed = 0; }
    if (event->param == BGMT_UNPRESS_ZOOM_OUT) { zoom_out_pressed = 0; zoom_in_pressed = 0; }
    #endif
    
    (void)zoom_in_pressed; /* silence warning */

    return 1;
}

void turn_on_display(void)
{
    #if defined(CONFIG_DIGIC_45)
    call("TurnOnDisplay");
    #elif defined(CONFIG_NO_DISPLAY_CALLS)
    extern void _turn_on_display(void);
    _turn_on_display();
    #endif
}

void turn_off_display(void)
{
    #if defined(CONFIG_DIGIC_45)
    call("TurnOffDisplay");
    #elif defined(CONFIG_NO_DISPLAY_CALLS)
    extern void _turn_off_display(void);
    _turn_off_display();
    #endif
}

// those functions seem not to be thread safe
// calling them from gui_main_task seems to sync them with other Canon calls properly
int handle_tricky_canon_calls(struct event *event)
{
    // fake ML events are always negative numbers
    if (event->param >= 0)
        return 1;

    //~ static int k; k++;
    //~ bmp_printf(FONT_LARGE, 50, 50, "[%d] tricky call: %d ", k, event->param); msleep(1000);

    switch (event->param)
    {
        #ifdef CONFIG_RESTORE_AFTER_FORMAT
        case MLEV_HIJACK_FORMAT_DIALOG_BOX:
            HijackFormatDialogBox();
            break;
        #endif
        case MLEV_TURN_ON_DISPLAY:
            if (!DISPLAY_IS_ON)
            {
                turn_on_display();
            }
            break;
        case MLEV_TURN_OFF_DISPLAY:
            if (DISPLAY_IS_ON)
            {
                turn_off_display();
            }
            break;
        /*case MLEV_ChangeHDMIOutputSizeToVGA:
            ChangeHDMIOutputSizeToVGA();
            break;*/
        case MLEV_LCD_SENSOR_START:
            #ifdef CONFIG_LCD_SENSOR
            DispSensorStart();
            #endif
            break;
        case MLEV_REDRAW:
            _redraw_do();   /* todo: move in gui-common.c */
            break;
    }
    
    return 1;
}

// engio functions may fail and lock the camera
void EngDrvOut(uint32_t reg, uint32_t value)
{
    if (ml_shutdown_requested) return;
#if defined(CONFIG_200D)
    // 200D doesn't seem to do this check.  Compare ConnectWriteEDmac() on 70D,
    // where it calls into ff2bc3cc(), passing some channel info ptr.  That func
    // only returns the val if the LCLK bit is set.
    //
    // On 200D, no check.  And EngDrvOut() is inlined.

    // I've left the default for unknown cams to do the read from 0xc040_0008.
    // The c000_0000 region is still mapped, so it should still be safe.
    // And if they do need the read to have 2 bit set, no write occurs if
    // the device is missing (probably?).

    // Most likely, all D678X cams don't want this read.
#else // Digic 45 (3?)
    if (!(MEM(0xC0400008) & 0x2)) return; // this routine requires LCLK enabled
#endif
    _EngDrvOut(reg, value);
}

void engio_write(uint32_t* reg_list)
{
    if (!(MEM(0xC0400008) & 0x2)) return; // this routine requires LCLK enabled
    _engio_write(reg_list);
}
