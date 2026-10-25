/**
 * Manual function overrides and ICALL diagnostics
 *
 * This file provides:
 *   - recomp_lookup_manual()  : intercept specific Xbox VAs with hand-written code
 *   - recomp_icall_fail_log() : log when an indirect call target can't be resolved
 *   - ICALL trace ring buffer  : globals used by the RECOMP_ICALL macro
 *
 * The recomp pipeline generates an auto-dispatch table (recomp_lookup) that
 * resolves most function addresses. recomp_lookup_manual() is called FIRST,
 * giving you a chance to override any function with a custom implementation.
 *
 * Common reasons to add manual overrides:
 *   - Trace a function to understand call flow (wrap the generated version)
 *   - Fix a function the lifter translated incorrectly
 *   - Stub out a function that crashes (return early, set eax to a safe value)
 *   - Redirect a function to a native implementation (e.g., skip CRT init)
 *   - Intercept D3D/audio calls for custom rendering or sound
 */

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>   /* ptrdiff_t */

/* ── ICALL trace ring buffer ───────────────────────────────── */

/*
 * These globals are written by the RECOMP_ICALL macro (defined in
 * recomp_types.h) every time an indirect call is dispatched. When a
 * crash occurs, the VEH handler or recomp_icall_fail_log() can dump
 * the last 16 call targets to help you trace what happened.
 *
 * The runtime owns them: xbox_kernel defines all three in
 * src/kernel/xbox_memory_layout.c, and recomp_types.h declares them extern.
 * Declare, do not define -- a definition here as well is a duplicate symbol,
 * and a project copied from this template failed to link on all three:
 *
 *   xbox_memory_layout.obj : error LNK2005: g_icall_count already defined
 *                            in recomp_manual.obj
 */
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;
extern volatile uint64_t g_icall_count;

typedef void (*recomp_func_t)(void);

/* ── Register state (defined in xbox_memory_layout.c) ──────── */

#include "recomp/gen/recomp_types.h"   /* thread-local register declarations, MEM32 */

/* ── Manual function overrides ─────────────────────────────── */

/*
 * Return a function pointer to override the given Xbox VA, or NULL
 * to fall through to the auto-generated dispatch table.
 *
 * This is called on every indirect call (RECOMP_ICALL) and every
 * direct call through the dispatch table, so keep it fast. A chain
 * of if-statements on uint32_t compiles to a simple comparison
 * sequence; for large override tables, consider a sorted array
 * with binary search.
 *
 * Examples of common override patterns:
 *
 *   // Trace wrapper: log entry/exit around the generated function
 *   extern void sub_00012345(void);
 *   static void traced_sub_00012345(void) {
 *       fprintf(stderr, "[TRACE] sub_00012345 entered, eax=0x%08X\n", g_eax);
 *       sub_00012345();
 *       fprintf(stderr, "[TRACE] sub_00012345 returned, eax=0x%08X\n", g_eax);
 *   }
 *
 *   // Stub: skip a function entirely (return 0 in eax)
 *   static void stub_00067890(void) {
 *       g_eax = 0;
 *   }
 *
 *   // Fix: replace a broken lifted function with correct C
 *   static void fixed_sub_000ABCDE(void) {
 *       // Read arguments from stack/registers per calling convention
 *       uint32_t arg1 = g_ecx;
 *       uint32_t arg2 = MEM32(g_esp + 4);
 *       // ... correct implementation ...
 *       g_eax = result;
 *   }
 */
/*
 * D3D_BlockOnTime (0x00344640): stdcall(fence, flags), ret 8.
 *
 * XDK D3D waits here for the GPU to consume the pushbuffer up to `fence`. It
 * polls the semaphore that dev+0x30 points at; nothing advances it, so the
 * title spins forever. Point it at the write sequence (dev+0x2C) so the GPU is
 * always "caught up", and skip the kick-off the original does first (there is
 * no pushbuffer executor running). Device pointer: MEM32(0x351F48).
 * See docs/technical/d3d8ltcg-device-context.md in xboxrecomp.
 * Defined as sub_X (a replacement), not sub_X_gen, so --exclude-manual makes
 * every generated caller reach it.
 *
 * With the pushbuffer executor on (RECOMP_PB_EXEC, which RECOMP_D3D11 implies)
 * "always caught up" is a lie the title acts on: the executor runs behind the
 * game thread, and D3D reuses dynamic vertex buffers (the deformable car body,
 * stencil shadow volumes) and ring space as soon as it believes the GPU is past
 * them. Those draws then read the next frame's half-written data -- the random
 * one-frame flicker (black wedges, missing car panels). So keep the real
 * semaphore: hand it to the executor, which writes the title's
 * BACK_END_WRITE_SEMAPHORE_RELEASE values there, and wait for the fence like
 * the original, bounded at 250 ms (xboxrecomp docs/technical/
 * d3d8ltcg-device-context.md, "When Something Does Read the Pushbuffer").
 * The original first inserts + kicks the fence when it is the newest one
 * (sub_00344410(0): fence = dev+0x2C, then dev+0x2C += 2).
 * RECOMP_D3D_FENCE_FAKE=1 restores the old behaviour.
 */
#include <stdlib.h>
extern void nv2a_pb_set_semaphore_target(uint32_t guest_va);
extern void sub_00344410(void);
__declspec(dllimport) unsigned long __stdcall GetTickCount(void);
__declspec(dllimport) int __stdcall SwitchToThread(void);

void sub_00344640(void)
{
    static int honest = -1;
    static uint32_t sem;            /* guest VA of the GPU semaphore word */
    uint32_t dev = MEM32(0x351F48);
    uint32_t fence = MEM32(g_esp + 4);

    if (honest < 0)
        honest = getenv("RECOMP_PB_EXEC") && !getenv("RECOMP_D3D_FENCE_FAKE");
    if (dev && honest && !sem) {
        uint32_t p = MEM32(dev + 0x30);
        if (p && p != dev + 0x2C) {
            sem = p;
            nv2a_pb_set_semaphore_target(sem);
            fprintf(stderr, "[D3D] GPU semaphore at 0x%08X (fence wait is honest)\n", sem);
        } else {
            honest = 0;
        }
    }
    if (!dev || !honest) {
        if (dev)
            MEM32(dev + 0x30) = dev + 0x2C;
        g_esp += 12; /* ret 8 */
        return;
    }
    {
        uint32_t seq = MEM32(dev + 0x2C);
        if (seq - fence < seq - MEM32(sem)) {             /* not passed yet */
            unsigned long t0 = GetTickCount();
            static unsigned long ntimeouts;
            if (fence == seq) {                           /* newest: insert and kick it */
                PUSH32(g_esp, 0);
                PUSH32(g_esp, 0x0034466Fu);
                sub_00344410();                           /* ret 4 */
            }
            for (;;) {
                seq = MEM32(dev + 0x2C);
                if (seq - fence >= seq - MEM32(sem))
                    break;
                if (GetTickCount() - t0 > 250) {
                    if (ntimeouts++ < 20)
                        fprintf(stderr, "[D3D] fence %08X wait timed out (semaphore %08X, seq %08X)\n",
                                fence, MEM32(sem), seq);
                    break;
                }
                SwitchToThread();
            }
        }
    }
    g_esp += 12; /* ret 8 */
}

/*
 * D3D overlay stop (0x003430A0): stdcall, 1 arg, ret 4.
 *
 * ctx = MEM32(dev+0x4F8) is the NV2A register base (0xFD000000), and +0x8700 /
 * +0x8704 are PVIDEO_BUFFER / PVIDEO_STOP. The original sets STOP if an overlay
 * buffer is in use and then spins until the hardware clears BUFFER. Bink plays
 * its video through that YUY2 overlay, and the emulated register file is plain
 * memory, so the bit never clears. Do the same register writes, but clear the
 * busy bit ourselves (what the hardware would do).
 */
void sub_003430A0(void)
{
    uint32_t dev = MEM32(0x351F48);
    uint32_t ctx = MEM32(dev + 0x4F8);
    if (MEM32(ctx + 0x8700))
        MEM32(ctx + 0x8704) = 1;
    MEM32(ctx + 0x8700) = 0;
    MEM32(ctx + 0x8100) = 0x11;
    MEM32(ctx + 0x8920) = 0;
    MEM32(ctx + 0x8928) = 0xFFFFFFFFu;
    MEM32(ctx + 0x8930) = 0;
    MEM32(ctx + 0x8900) = 0;
    MEM32(ctx + 0x8910) = 0x1000;
    MEM32(ctx + 0x8918) = 0x1000;
    g_eax = ctx;
    g_esp += 8; /* ret 4 */
}

/* ── XAPI gamepad (XPP section) replaced at the API level ─────────────────
 *
 * The XDK's input library drives a real USB stack: XInitDevices starts the
 * hub/OHCI code, which enumerates the pad, fills in the "connected" masks of
 * the XPP_DEVICE_TYPE structs, and XInputOpen/XInputGetState talk to the device.
 * With the emulated OHCI controller that enumeration never completes, so the
 * game never sees a pad and stays in attract mode. The game's own code is clear
 * about what it needs (sub_001A3EA9: XGetDevices(gamepad type) every frame, then
 * XInputOpen/GetState per port), so these are answered here from the toolkit's
 * pad report (usb_gamepad_report: host XInput pad, RECOMP_KEYBOARD, and the
 * RECOMP_PAD_PRESS / RECOMP_PAD_SCRIPT test inputs).
 *
 * 0x3592B8 is XDEVICE_TYPE_GAMEPAD in this binary.
 */
extern int usb_gamepad_report(int pad, uint8_t *out, int max);
#include <string.h>
#include <stdlib.h>

/* RECOMP_PAD_TRACE=1: log the first few calls of each overridden XAPI input
 * function (which of them the game really uses, and with what). */
static int mm3_pad_trace(int idx, const char *what, uint32_t a, uint32_t b)
{
    static int on = -1;
    static unsigned long n[8], lastprint[8];
    unsigned long now;
    if (on < 0) { const char *e = getenv("RECOMP_PAD_TRACE"); on = e && *e && *e != '0'; }
    if (!on) return 0;
    n[idx]++;
    now = (unsigned long)GetTickCount();
    /* first 3 calls, then at most one line per 5 s with the running count */
    if (n[idx] > 3 && now - lastprint[idx] < 5000) return 0;
    lastprint[idx] = now;
    fprintf(stderr, "  [PADAPI] %s #%lu (0x%08X, 0x%08X)\n", what, n[idx], a, b);
    fflush(stderr);
    return 1;
}

#define MM3_XDEVICE_TYPE_GAMEPAD 0x003592B8u
#define MM3_PAD_HANDLE           0x00360100u   /* opaque cookie; only handed back */

/* DWORD XGetDevices(PXPP_DEVICE_TYPE): stdcall, ret 4. Original returns
 * type[0] (connected mask) and rolls type[4] / type[8]. */
void sub_0035A81D(void)
{
    uint32_t t = MEM32(g_esp + 4);
    uint32_t mask = (t == MM3_XDEVICE_TYPE_GAMEPAD) ? 1u : MEM32(t);
    mm3_pad_trace(0, "XGetDevices", t, mask);
    MEM32(t + 4) = 0;
    MEM32(t + 8) = mask;
    g_eax = mask;
    g_esp += 8; /* ret 4 */
}

/* HANDLE XInputOpen(type, port, slot, pPollingParameters): stdcall, ret 16. */
void sub_00361BCF(void)
{
    uint32_t t = MEM32(g_esp + 4);
    uint32_t port = MEM32(g_esp + 8);
    mm3_pad_trace(1, "XInputOpen", t, port);
    g_eax = (t == MM3_XDEVICE_TYPE_GAMEPAD && port == 0) ? MM3_PAD_HANDLE : 0;
    g_esp += 20; /* ret 16 */
}

/* void XInputClose(HANDLE): stdcall, ret 4. */
void sub_00361C25(void)
{
    mm3_pad_trace(2, "XInputClose", MEM32(g_esp + 4), 0);
    g_eax = 0;
    g_esp += 8; /* ret 4 */
}

/* DWORD XInputGetState(HANDLE, PXINPUT_STATE): stdcall, ret 8. (0x00361E0F:
 * the per-frame update, sub_001A6278, passes esi+0x10 and then reads the
 * gamepad at esi+0x14, i.e. packet number + gamepad.)
 * XINPUT_STATE: DWORD packet; WORD wButtons; BYTE analog[8] (A B X Y Black
 * White LT RT); SHORT LX, LY, RX, RY. The USB report carries the same bytes at
 * 2 (digital), 4..11 (analog) and 12..19 (sticks). */
void sub_00361E0F(void)
{
    static uint32_t packet;
    static uint8_t last[20];
    uint32_t st = MEM32(g_esp + 8);
    uint8_t r[20];
    int i;

    memset(r, 0, sizeof r);
    usb_gamepad_report(0, r, 20);
    mm3_pad_trace(3, "XInputGetState", MEM32(g_esp + 4), (uint32_t)r[2] | ((uint32_t)r[4] << 8));
    if (memcmp(r + 2, last + 2, 18) != 0) {
        packet++;
        memcpy(last, r, sizeof last);
    }
    MEM32(st) = packet;
    MEM16(st + 4) = (uint16_t)r[2];
    for (i = 0; i < 8; i++)
        MEM8(st + 6 + i) = r[4 + i];
    for (i = 0; i < 4; i++)
        MEM16(st + 14 + 2 * i) = (uint16_t)(r[12 + 2 * i] | (r[13 + 2 * i] << 8));
    g_eax = 0; /* ERROR_SUCCESS */
    g_esp += 12; /* ret 8 */
}

/* DWORD XInputGetCapabilities(HANDLE, PXINPUT_CAPABILITIES): stdcall, ret 8.
 * (0x00361C31: called once from XInputOpen's caller, sub_001A616E, with a
 * scratch area at esi+0x6C.) */
void sub_00361C31(void)
{
    uint32_t caps = MEM32(g_esp + 8);
    mm3_pad_trace(4, "XInputGetCapabilities", MEM32(g_esp + 4), caps);
    MEM8(caps) = 1; /* SubType: gamepad */
    g_eax = 0;
    g_esp += 12; /* ret 8 */
}

/* DWORD XInputSetState(HANDLE, PXINPUT_FEEDBACK): stdcall, ret 8 (rumble).
 * XINPUT_FEEDBACK is packed: header (DWORD dwStatus, HANDLE hEvent, BYTE
 * Reserved[58]) then XINPUT_RUMBLE at +0x42 (left, low-frequency motor) and
 * +0x44 (right). sub_001A60A6 fills those from its motor floats (esi+0x88 /
 * esi+0x8C) once per frame. Forwarded to host pad 0 through the toolkit
 * (xbox_InputSetState: XInput or SDL), only when the speeds change;
 * RECOMP_RUMBLE=0 turns it off, as for the OHCI path.
 * Report it complete: the game skips a new request while dwStatus is
 * ERROR_IO_PENDING (0x3E5). */
typedef struct { uint16_t left, right; } mm3_vibration;
extern unsigned long xbox_InputSetState(unsigned long port, const mm3_vibration *v);

static void mm3_rumble_off(void)
{
    mm3_vibration v = { 0, 0 };
    xbox_InputSetState(0, &v);
}

void sub_00361E7B(void)
{
    static int on = -1;
    static mm3_vibration last;
    uint32_t fb = MEM32(g_esp + 8);
    mm3_vibration v;

    if (on < 0) {
        const char *e = getenv("RECOMP_RUMBLE");
        on = !(e && *e == '0');
        if (on)
            atexit(mm3_rumble_off);   /* XInput keeps the motors running */
    }
    v.left = MEM16(fb + 0x42);
    v.right = MEM16(fb + 0x44);
    mm3_pad_trace(5, "XInputSetState", v.left, v.right);
    /* a pad that was unplugged gets the current speeds once it is back */
    if (on && (v.left != last.left || v.right != last.right)
        && xbox_InputSetState(0, &v) == 0)
        last = v;
    MEM32(fb) = 0;
    g_eax = 0;
    g_esp += 12; /* ret 8 */
}

/*
 * XNet startup/status wrapper (0x002AF497): stdcall, 1 arg, ret 4.
 *
 * The game's network check (sub_00195341) calls this, then polls an async
 * operation with Sleep(100) until it completes. There is no NIC, so it never
 * does and the loading screen hangs forever. A non-zero result makes the check
 * take its "no network" exit straight away.
 */
void sub_002AF497(void)
{
    g_eax = 1;
    g_esp += 8; /* ret 4 */
}

/*
 * XNet receive callback (0x002B2314): stdcall, 4 args, ret 16.
 *
 * It passes a received frame to the XNet stack, which divides by a field of
 * the network interface object ([iface+0xAD4]) that is only set once the NIC
 * is up. There is no NIC here, so that is zero and the worker thread dies of
 * a divide error (0xC0000094). Networking is not needed to reach the menus:
 * drop the frame.
 */
void sub_002B2314(void)
{
    g_eax = 0;
    g_esp += 20; /* ret 16 */
}

/* ── RECOMP_DSTRACE=1: trace indirect calls into DirectSound / XACT / WMA code ──
 * Logs the first few calls of each function (this, four stack args, eax on return).
 * Indirect calls only: the game reaches those libraries through COM vtables. */
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_ecx;

static void dst_call(uint32_t va)
{
    static uint32_t seen[4096];
    static uint32_t cnt[4096];
    static uint32_t total;
    unsigned h = (va * 2654435761u) >> 20, tries = 0;
    int log;
    uint32_t esp0 = g_esp, ecx0 = g_ecx;
    recomp_func_t f = recomp_lookup(va);

    while (seen[h] && seen[h] != va && tries++ < 4096)
        h = (h + 1) & 4095;
    seen[h] = va;
    log = cnt[h] < 6;
    cnt[h]++;
    if (++total % 4000 == 0) {
        unsigned q;
        fprintf(stderr, "[DST-COUNTS] total %u:", total);
        for (q = 0; q < 4096; q++)
            if (seen[q])
                fprintf(stderr, " %06X=%u", seen[q], cnt[q]);
        fprintf(stderr, "\n");
    }
    if (!f)
        return;
    if (log)
        fprintf(stderr, "[DST] %08X this=%08X args %08X %08X %08X %08X\n", va, ecx0,
                MEM32(esp0 + 4), MEM32(esp0 + 8), MEM32(esp0 + 12), MEM32(esp0 + 16));
    f();
    if (log)
        fprintf(stderr, "[DST] %08X -> eax=%08X\n", va, g_eax);
    if (va == 0x002772FA && cnt[h] > 4 && cnt[h] < 400) {
        uint32_t s = MEM32(esp0 + 4), d = MEM32(esp0 + 8), in = MEM32(s + 0x24);
        fprintf(stderr, "[DST-PROC] #%u s=%08X flags12=%04X ret=%08X desc=%08X: %08X %08X %08X %08X %08X %08X\n",
                cnt[h], s, MEM16(in + 0x12), g_eax, d, MEM32(d), MEM32(d + 4), MEM32(d + 8), MEM32(d + 12),
                MEM32(d + 16), MEM32(d + 20));
    }
    if (va == 0x002772A9 && (cnt[h] % 100) == 1) {
        uint32_t s = MEM32(esp0 + 4), in = MEM32(s + 0x24), out = MEM32(esp0 + 8);
        fprintf(stderr, "[DST-STREAM] #%u s=%08X in=%08X flags12=%04X out=%08X list@B8: %08X %08X status=%08X\n",
                cnt[h], s, in, MEM16(in + 0x12), out, MEM32(in + 0xB8), MEM32(in + 0xBC), MEM32(out));
    }
}

#include "dstrace.inc"

recomp_func_t recomp_lookup_manual(uint32_t xbox_va)
{
    /*
     * TODO: Add your overrides here. Examples:
     *
     * if (xbox_va == 0x00012345) return traced_sub_00012345;
     * if (xbox_va == 0x00067890) return stub_00067890;
     * if (xbox_va == 0x000ABCDE) return fixed_sub_000ABCDE;
     */

    {
        static int dst = -1;
        if (dst < 0) dst = getenv("RECOMP_DSTRACE") != NULL;
        if (dst)
            return dst_lookup(xbox_va);
    }
    return (recomp_func_t)0;
}

/* ── ICALL failure logging ─────────────────────────────────── */

/*
 * Called when RECOMP_ICALL cannot resolve a target address.
 * This usually means one of:
 *   - A vtable dispatch to an address not in the dispatch table
 *   - A function pointer loaded from uninitialized or corrupt memory
 *   - A kernel thunk address that the bridge doesn't handle
 *
 * During early bring-up you will see many of these. Most are harmless
 * (the ICALL macro pops the dummy return address and continues).
 * Focus on the ones that cause crashes or incorrect behavior.
 */
void recomp_icall_fail_log(uint32_t va)
{
    fprintf(stderr, "[ICALL] Failed to resolve VA 0x%08X (total calls: %llu)\n",
            va, (unsigned long long)g_icall_count);

    /* Dump last 16 call targets from the ring buffer */
    fprintf(stderr, "  Recent ICALL targets:\n");
    for (int i = 0; i < 16; i++) {
        int idx = (g_icall_trace_idx - 16 + i) & 15;
        if (g_icall_trace[idx])
            fprintf(stderr, "    [%2d] 0x%08X\n", i, g_icall_trace[idx]);
    }
    fflush(stderr);
}

/* An indirect call whose target is not code: a null or wild function pointer.
 *
 * Skipping these is right -- calling a data address is worse -- but skipping
 * them *silently* is not. They almost always arrive inside a loop, so the
 * symptom is a hang with no output rather than a diagnosable null vtable call.
 *
 * Rate-limited per address: a spin can produce millions of these, and the
 * useful information is which addresses occur, not how often.
 */
void recomp_icall_not_code_log(uint32_t va)
{
    enum { SLOTS = 16 };
    static uint32_t seen[SLOTS];
    static uint64_t hits[SLOTS];
    static int count;
    int i;

    for (i = 0; i < count; i++)
        if (seen[i] == va)
            break;
    if (i == count) {
        if (count == SLOTS)
            return;
        seen[count] = va;
        hits[count] = 0;
        count++;
    }
    hits[i]++;
    /* Report at 1, 10, 100, 1000 ... rather than once. A single line says a
     * wild pointer was skipped; the progression says it is being skipped in a
     * loop, which is the difference between a curiosity and the reason the
     * title is hung. */
    {
        uint64_t n = hits[i];
        while (n >= 10 && n % 10 == 0)
            n /= 10;
        if (n != 1)
            return;
    }
    fprintf(stderr, "[ICALL] target 0x%08X is not code -- skipped %llu time(s) "
                    "(null or wild function pointer, at call #%llu)\n",
            va, (unsigned long long)hits[i],
            (unsigned long long)g_icall_count);
    fflush(stderr);
}

/* ── Untranslated instructions ───────────────────────────────────────────
 *
 * The lifter emits RECOMP_UNIMPL(text, va) at every instruction it has no
 * translation for, in place of the bare comment it used to leave. The
 * instruction is still a no-op; this only stops the omission being silent.
 * RECOMP_UNIMPL_TRAP=1 aborts at the first hit, at the guest address of the
 * cause rather than wherever the damage surfaces. */
#include <stdlib.h>

void recomp_unimpl(const char *text, uint32_t va)
{
    static int printed;
    const char *trap = getenv("RECOMP_UNIMPL_TRAP");
    int stop = trap && *trap && *trap != '0';

    if (printed < 50 || stop) {
        printed++;
        fprintf(stderr,
                "[UNIMPL] untranslated instruction REACHED: `%s` at 0x%08X"
                " (a no-op; set RECOMP_UNIMPL_TRAP=1 to stop here)\n",
                text, va);
        fflush(stderr);
    }
    if (stop) abort();
}

/*
 * Pedestrian LOD (patched in by patch_gen.py, sub_000DB5BB at 0xDB6DF).
 *
 * MM3 picks a pedestrian's LOD by distance from the camera against the table that
 * Scripts/ai_init_common.lua sets (renderPedestrianLodDistance 50,100,120,130,160).
 * Levels 0-3 are skinned; level 4+ (beyond 130 m) uses a rigid draw whose constant
 * provider (sub_000DA071) turns the body matrix 90 degrees about X and lowers it
 * 0.9 m -- a pelvis-to-body correction -- but it is handed the plain upright body
 * matrix, so every far pedestrian lies flat. Drawing level 4+ through the level 1-3
 * skinned path keeps them standing. RECOMP_PED_RIGID=1 restores the rigid draw.
 */
int mm3_ped_rigid_lod(void)
{
    static int rigid = -1;
    if (rigid < 0)
        rigid = getenv("RECOMP_PED_RIGID") != NULL;
    return rigid;
}
