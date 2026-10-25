/*
 * RECOMP_SAMPLE=1: a sampling profiler for the game thread, to find what the
 * title is doing in slow frames (NOTES 35).
 *
 * A thread suspends the game thread every millisecond and records where it
 * is (instruction pointer plus the caller found by unwinding one frame).
 * Samples collect per frame; the back end's flip says whether the frame was
 * slow (longer than RECOMP_SAMPLE_SLOW_MS, default 45) and the frame's
 * samples go to the "slow" or the "normal" table. Every 60 s the top
 * functions of both tables are printed as [SAMPLE] lines, resolved through
 * the PDB (generated functions are sub_<guest address>).
 */
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define TAB_SIZE 65536          /* open addressing, power of two */
#define FRAME_MAX 4096
#define DEPTH 12

typedef struct { DWORD64 pc, caller; DWORD64 stack[DEPTH]; uint32_t slow, normal; } SampleEntry;

static SampleEntry *s_tab;
static CRITICAL_SECTION s_lock;
static HANDLE s_target;
static DWORD64 s_frame_pc[FRAME_MAX], s_frame_caller[FRAME_MAX], s_frame_stack[FRAME_MAX][DEPTH];
static int s_frame_n;
static uint32_t s_slow_frames, s_normal_frames, s_slow_ms;
static volatile LONG s_on;

/* Keyed by pc, caller and the deeper stack, so one leaf reached two ways stays two entries. */
static SampleEntry *tab_get(DWORD64 pc, DWORD64 caller, const DWORD64 *stack)
{
    uint64_t hs = pc * 0x9E3779B97F4A7C15ull ^ caller;
    uint32_t i, k, h;
    for (k = 0; k < DEPTH; k++) hs = hs * 1099511628211ull ^ stack[k];
    h = (uint32_t)(hs >> 32) ^ (uint32_t)hs;
    for (k = 0; k < TAB_SIZE; k++) {
        i = (h + k) & (TAB_SIZE - 1);
        if (!s_tab[i].pc) {
            s_tab[i].pc = pc; s_tab[i].caller = caller;
            memcpy(s_tab[i].stack, stack, sizeof s_tab[i].stack);
            return &s_tab[i];
        }
        if (s_tab[i].pc == pc && s_tab[i].caller == caller && !memcmp(s_tab[i].stack, stack, sizeof s_tab[i].stack))
            return &s_tab[i];
    }
    return NULL;
}

static void name_of(DWORD64 a, char *out, size_t n)
{
    char buf[sizeof(SYMBOL_INFO) + 256];
    SYMBOL_INFO *si = (SYMBOL_INFO *)buf;
    DWORD64 disp = 0;
    memset(buf, 0, sizeof buf);
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 255;
    if (a && SymFromAddr(GetCurrentProcess(), a, &disp, si))
        snprintf(out, n, "%s", si->Name);
    else
        snprintf(out, n, "%p", (void *)a);
}

/* Aggregate by (function, caller function) name and print the top entries. */
typedef struct { char fn[96], caller[96], via[200]; uint32_t slow, normal; } Agg;

static int cmp_slow(const void *a, const void *b)
{
    const Agg *x = (const Agg *)a, *y = (const Agg *)b;
    return (int)y->slow - (int)x->slow;
}
static int cmp_normal(const void *a, const void *b)
{
    const Agg *x = (const Agg *)a, *y = (const Agg *)b;
    return (int)y->normal - (int)x->normal;
}

static void report(void)
{
    Agg *agg;
    int na = 0, i, j;
    uint32_t tot_slow = 0, tot_normal = 0;
    SampleEntry *copy = (SampleEntry *)malloc(sizeof(SampleEntry) * TAB_SIZE);
    if (!copy) return;
    EnterCriticalSection(&s_lock);
    memcpy(copy, s_tab, sizeof(SampleEntry) * TAB_SIZE);
    LeaveCriticalSection(&s_lock);
    agg = (Agg *)calloc(8192, sizeof(Agg));
    if (!agg) { free(copy); return; }
    for (i = 0; i < TAB_SIZE; i++) {
        char fn[96], cl[96];
        if (!copy[i].pc) continue;
        tot_slow += copy[i].slow; tot_normal += copy[i].normal;
        name_of(copy[i].pc, fn, sizeof fn);
        char via[200] = "";
        int k, got_host = 0;
        name_of(copy[i].caller, cl, sizeof cl);
        /* Deeper: the first non-API host function and the first guest function. */
        for (k = 0; k < DEPTH && copy[i].stack[k]; k++) {
            char nm[96];
            name_of(copy[i].stack[k], nm, sizeof nm);
            if (!got_host && strncmp(nm, "sub_", 4) && strncmp(nm, "Nt", 2) && strncmp(nm, "Zw", 2)
                && strstr(nm, "File") == NULL && strstr(nm, "0x") == NULL && nm[0] != '0') {
                strncat(via, nm, sizeof via - strlen(via) - 2); strncat(via, " ", 2); got_host = 1;
            }
            if (!strncmp(nm, "sub_", 4)) { strncat(via, nm, sizeof via - strlen(via) - 1); break; }
        }
        for (j = 0; j < na; j++)
            if (!strcmp(agg[j].fn, fn) && !strcmp(agg[j].caller, cl) && !strcmp(agg[j].via, via)) break;
        if (j == na) {
            if (na == 8192) continue;
            strcpy(agg[na].fn, fn); strcpy(agg[na].caller, cl); strcpy(agg[na].via, via); na++;
        }
        agg[j].slow += copy[i].slow; agg[j].normal += copy[i].normal;
    }
    fprintf(stderr, "[SAMPLE] %u slow frames (%u samples), %u normal frames (%u samples)\n",
            s_slow_frames, tot_slow, s_normal_frames, tot_normal);
    qsort(agg, na, sizeof *agg, cmp_slow);
    for (i = 0; i < na && i < 25; i++)
        fprintf(stderr, "[SAMPLE] slow %5.1f%% normal %5.1f%%  %s <- %s  [%s]\n",
                tot_slow ? 100.0 * agg[i].slow / tot_slow : 0.0,
                tot_normal ? 100.0 * agg[i].normal / tot_normal : 0.0, agg[i].fn, agg[i].caller, agg[i].via);
    qsort(agg, na, sizeof *agg, cmp_normal);
    for (i = 0; i < na && i < 10; i++)
        fprintf(stderr, "[SAMPLE] (normal top) normal %5.1f%% slow %5.1f%%  %s <- %s\n",
                tot_normal ? 100.0 * agg[i].normal / tot_normal : 0.0,
                tot_slow ? 100.0 * agg[i].slow / tot_slow : 0.0, agg[i].fn, agg[i].caller);
    fflush(stderr);
    free(agg);
    free(copy);
}

static DWORD WINAPI sampler_thread(void *arg)
{
    ULONGLONG next_report = GetTickCount64() + 60000;
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    timeBeginPeriod(1);
    for (;;) {
        CONTEXT ctx;
        DWORD64 pc = 0, caller = 0, stack[DEPTH];
        int d;
        memset(stack, 0, sizeof stack);
        Sleep(1);
        if (SuspendThread(s_target) == (DWORD)-1)
            break;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(s_target, &ctx)) {
#ifdef _WIN64
            DWORD64 base = 0;
            PRUNTIME_FUNCTION rf;
            pc = ctx.Rip;
            rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
            if (rf) {
                void *hd = NULL;
                DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &ef, NULL);
                caller = ctx.Rip;
            } else {
                caller = *(DWORD64 *)ctx.Rsp;       /* leaf: return address on top */
                ctx.Rip = caller;
                ctx.Rsp += 8;
            }
            for (d = 0; d < DEPTH && ctx.Rip; d++) {
                stack[d] = ctx.Rip;
                rf = RtlLookupFunctionEntry(ctx.Rip, &base, NULL);
                if (!rf) break;
                {
                    void *hd = NULL;
                    DWORD64 ef = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &ef, NULL);
                }
            }
#else
            pc = ctx.Eip;
#endif
        }
        ResumeThread(s_target);
        if (pc) {
            EnterCriticalSection(&s_lock);
            if (s_frame_n < FRAME_MAX) {
                s_frame_pc[s_frame_n] = pc;
                s_frame_caller[s_frame_n] = caller;
                memcpy(s_frame_stack[s_frame_n], stack, sizeof stack);
                s_frame_n++;
            }
            LeaveCriticalSection(&s_lock);
        }
        if (GetTickCount64() >= next_report) {
            next_report = GetTickCount64() + 60000;
            report();
        }
    }
    return 0;
}

/* Called by the back end at each flip with the frame's length. */
void mm3_sampler_frame(double ms)
{
    int i, slow;
    if (!s_on) return;
    slow = ms > (double)s_slow_ms;
    EnterCriticalSection(&s_lock);
    for (i = 0; i < s_frame_n; i++) {
        SampleEntry *e = tab_get(s_frame_pc[i], s_frame_caller[i], s_frame_stack[i]);
        if (e) { if (slow) e->slow++; else e->normal++; }
    }
    s_frame_n = 0;
    if (slow) s_slow_frames++; else s_normal_frames++;
    LeaveCriticalSection(&s_lock);
}

/* Call on the game thread, before the title's entry point. */
void mm3_sampler_start(void)
{
    const char *e = getenv("RECOMP_SAMPLE");
    const char *sm = getenv("RECOMP_SAMPLE_SLOW_MS");
    if (!e || e[0] == '0') return;
    s_slow_ms = sm ? (uint32_t)atoi(sm) : 45;
    s_tab = (SampleEntry *)calloc(TAB_SIZE, sizeof(SampleEntry));
    if (!s_tab) return;
    InitializeCriticalSection(&s_lock);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &s_target,
                    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, 0);
    InterlockedExchange(&s_on, 1);
    CreateThread(NULL, 0, sampler_thread, NULL, 0, NULL);
    fprintf(stderr, "[SAMPLE] sampling the game thread every 1 ms, slow frame > %u ms\n", s_slow_ms);
}
