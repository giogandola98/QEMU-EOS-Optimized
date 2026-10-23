/*
 * wakeups: QEMU TCG plugin (QEMU 4.2 plugin API) that measures how often an ARMv5 guest leaves idle.
 * It needs no ROM-specific address, so it works for any DryOS camera and for the bare-metal self-test.
 *
 * Counted:
 *   wfi    executions of a wait-for-interrupt: `mcr p15,0,Rd,c7,c0,4` (ARM926/946, the DryOS idle loop),
 *          the other pre-v7 encodings QEMU/qemu-eos treat as WFI (c7,c8,2 and c15,c8,2) and the v6K hint.
 *          Every idle entry ends with one wakeup, so wfi/s = wakeups/s.
 *   irq    executions of the IRQ vector (0x18 or high vector 0xFFFF0018) = IRQ exception entries.
 *   fiq    same for the FIQ vector (0x1C / 0xFFFF001C).
 *   insns  guest instructions (counted per executed translation block, so exact unless a block is
 *          left early by a synchronous exception). insns=off drops this counter.
 *   halts  times the vCPU really slept (a WFI with an interrupt already pending returns at once),
 *          and the host time spent halted.
 *
 * Cost: everything on the guest's hot path is inline TCG code emitted at translation time (one add per
 * executed TB for insns, one add on each WFI / vector instruction). There are no per-instruction helper
 * calls, so the plugin adds no call/return or register spills to translated code. halts come from the
 * vCPU idle/resume hooks (once per real sleep). The measurement window and the interval records are cut
 * by a sampler thread on host time, independent of guest events.
 * burst=on adds one callback on WFI instructions only, to track the longest run of instructions between
 * two WFIs (max_insns_between_wfi; null when off). A WFI ends in a halt anyway, so this is cheap.
 *
 * Derived: insns between WFIs (mean; max with burst=on) and a busy ratio. With real-time QEMU (no -icount)
 * busy_ratio = 1 - halted/elapsed host time; TCG is slower than the real CPU, so this over-estimates
 * busy time. With icount_shift=N (same N as `-icount shift=N`) busy time is insns * 2^N ns over the
 * elapsed time instead, independent of the host speed. -icount forces single-threaded TCG, where QEMU 4.2
 * calls no idle/resume plugin callbacks, so halts/halted_ms stay 0 in that mode.
 *
 * Options (-plugin libwakeups.so,arg=KEY=VALUE,...):
 *   out=PATH          JSON lines output (default: stderr)
 *   start_ms=N        measurement window starts N ms (host time) after plugin load; earlier events
 *                     (boot) are not in the summary (default 0)
 *   interval_ms=N     also emit one "interval" record every N ms (default 0: summary only)
 *   icount_shift=N    see busy ratio above (default: unset)
 *   insns=on|off      count guest instructions (default on)
 *   burst=on|off      track max_insns_between_wfi (default off)
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

struct counters {
    uint64_t wfi, irq, fiq, insns, halts, halted_ns;
};

/* Written by the (single) vCPU thread: by inline TCG adds or in its idle/resume/WFI callbacks.
 * Read by the sampler thread with relaxed atomic loads: aligned 64-bit loads cannot tear on the
 * 64-bit hosts we run on, and a sample that is off by one in-flight increment does not matter. */
static struct counters now;
static uint64_t halt_start_ns;          /* 0 while running */
static uint64_t max_burst, insns_at_last_wfi;
static bool have_wfi;

static uint64_t t0_ns, start_ns, interval_ns;
static int icount_shift = -1;
static bool count_insns = true, track_burst;
static FILE *out;

/* Sampler state (sampler thread; the atexit hook after joining it). */
static struct counters win, ivl;
static uint64_t win_t_ns, ivl_t_ns;
static bool win_started;
static pthread_t sampler;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake;
static bool stopping;

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec - t0_ns;
}

#define LOAD(x) __atomic_load_n(&(x), __ATOMIC_RELAXED)

/* Consistent-enough copy of the counters at host time t, including a halt still in progress. */
static struct counters snapshot(uint64_t t)
{
    struct counters c = {
        .wfi = LOAD(now.wfi), .irq = LOAD(now.irq), .fiq = LOAD(now.fiq), .insns = LOAD(now.insns),
        .halts = LOAD(now.halts), .halted_ns = LOAD(now.halted_ns),
    };
    uint64_t hs = LOAD(halt_start_ns);

    if (hs && t > hs) {
        c.halted_ns += t - hs;
    }
    return c;
}

static void emit(const char *type, const struct counters *from, uint64_t t_from,
                 const struct counters *to, uint64_t t_to)
{
    struct counters d = {
        .wfi = to->wfi - from->wfi, .irq = to->irq - from->irq, .fiq = to->fiq - from->fiq,
        .insns = to->insns - from->insns, .halts = to->halts - from->halts,
        .halted_ns = to->halted_ns - from->halted_ns,
    };
    double s = (t_to - t_from) / 1e9;
    double busy;
    char maxb[24] = "null";

    if (icount_shift >= 0) {
        double busy_ns = (double)d.insns * (double)(1ull << icount_shift);
        busy = t_to > t_from ? busy_ns / (t_to - t_from) : 0;
        if (busy > 1) {
            busy = 1;
        }
    } else {
        busy = t_to > t_from ? 1.0 - (double)d.halted_ns / (t_to - t_from) : 0;
    }
    if (track_burst) {
        snprintf(maxb, sizeof(maxb), "%" PRIu64, LOAD(max_burst));
    }
    fprintf(out,
            "{\"type\":\"%s\",\"t_start_ms\":%.1f,\"t_end_ms\":%.1f,\"seconds\":%.3f,"
            "\"wfi\":%" PRIu64 ",\"irq\":%" PRIu64 ",\"fiq\":%" PRIu64 ",\"insns\":%" PRIu64 ","
            "\"halts\":%" PRIu64 ",\"halted_ms\":%.1f,"
            "\"wakeups_per_s\":%.2f,\"irq_per_s\":%.2f,\"insns_per_s\":%.0f,"
            "\"insns_per_wakeup\":%.1f,\"max_insns_between_wfi\":%s,"
            "\"busy_ratio\":%.4f,\"busy_model\":\"%s\"}\n",
            type, t_from / 1e6, t_to / 1e6, s,
            d.wfi, d.irq, d.fiq, d.insns, d.halts, d.halted_ns / 1e6,
            s > 0 ? d.wfi / s : 0, s > 0 ? d.irq / s : 0, s > 0 ? d.insns / s : 0,
            d.wfi ? (double)d.insns / d.wfi : 0, maxb,
            busy < 0 ? 0 : busy, icount_shift >= 0 ? "icount" : "host_time");
    fflush(out);
}

static void start_window(uint64_t t)
{
    win = ivl = snapshot(t);
    win_t_ns = ivl_t_ns = t;
    __atomic_store_n(&max_burst, 0, __ATOMIC_RELAXED);  /* max is per window, boot bursts excluded */
    win_started = true;
}

/* Sleeps until the window start, then until each interval deadline (or forever without intervals). */
static void *sampler_main(void *arg)
{
    uint64_t next = start_ns;

    pthread_mutex_lock(&lock);
    while (!stopping) {
        uint64_t t = mono_ns();
        if (t < next || (win_started && !interval_ns)) {
            if (win_started && !interval_ns) {
                pthread_cond_wait(&wake, &lock);
            } else {
                uint64_t abs = next + t0_ns;
                struct timespec ts = { .tv_sec = abs / 1000000000u, .tv_nsec = abs % 1000000000u };
                pthread_cond_timedwait(&wake, &lock, &ts);
            }
            continue;
        }
        if (!win_started) {
            start_window(t);
        } else {
            struct counters c = snapshot(t);
            emit("interval", &ivl, ivl_t_ns, &c, t);
            ivl = c;
            ivl_t_ns = t;
        }
        next = t + interval_ns;
    }
    pthread_mutex_unlock(&lock);
    return NULL;
}

static void on_wfi_burst(unsigned int vcpu, void *udata)
{
    uint64_t insns = now.insns, burst = insns - insns_at_last_wfi;

    if (have_wfi && burst > max_burst) {
        __atomic_store_n(&max_burst, burst, __ATOMIC_RELAXED);
    }
    have_wfi = true;
    insns_at_last_wfi = insns;
}

static void on_idle(qemu_plugin_id_t id, unsigned int vcpu)
{
    __atomic_store_n(&now.halts, now.halts + 1, __ATOMIC_RELAXED);
    __atomic_store_n(&halt_start_ns, mono_ns(), __ATOMIC_RELAXED);
}

static void on_resume(qemu_plugin_id_t id, unsigned int vcpu)
{
    uint64_t hs = halt_start_ns;

    if (hs) {
        /* A snapshot taken between these two stores misses this halt; the next one includes it,
         * so only where one interval ends shifts, never the totals. */
        __atomic_store_n(&halt_start_ns, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&now.halted_ns, now.halted_ns + (mono_ns() - hs), __ATOMIC_RELAXED);
    }
}

static bool is_wfi(uint32_t w)
{
    if ((w >> 28) == 0xF) {
        return false;                       /* MCR2 / unconditional space */
    }
    switch (w & 0x0FFF0FFF) {               /* MCR p15, opc1=0, CRn, Rd=any, opc2, CRm */
    case 0x0E070F90:                        /* c7, c0, 4: ARM926/946 WFI, DryOS idle */
    case 0x0E070F58:                        /* c7, c8, 2: older v5 cores */
    case 0x0E0F0F58:                        /* c15, c8, 2: qemu-eos variant */
        return true;
    }
    return (w & 0x0FFFFFFF) == 0x0320F003;  /* ARMv6K+ WFI hint */
}

static void on_tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb *tb)
{
    size_t n = qemu_plugin_tb_n_insns(tb);

    if (count_insns) {
        qemu_plugin_register_vcpu_tb_exec_inline(tb, QEMU_PLUGIN_INLINE_ADD_U64, &now.insns, n);
    }
    for (size_t i = 0; i < n; i++) {
        struct qemu_plugin_insn *insn = qemu_plugin_tb_get_insn(tb, i);
        uint64_t va = qemu_plugin_insn_vaddr(insn);
        uint32_t w;

        if (va == 0x18 || va == 0xFFFF0018) {
            qemu_plugin_register_vcpu_insn_exec_inline(insn, QEMU_PLUGIN_INLINE_ADD_U64, &now.irq, 1);
        } else if (va == 0x1C || va == 0xFFFF001C) {
            qemu_plugin_register_vcpu_insn_exec_inline(insn, QEMU_PLUGIN_INLINE_ADD_U64, &now.fiq, 1);
        }
        /* Only 4-byte ARM instructions can be WFI on ARMv5 (Thumb-1 has no MCR). */
        if (qemu_plugin_insn_size(insn) == 4) {
            memcpy(&w, qemu_plugin_insn_data(insn), 4);    /* guest and host are little-endian */
            if (is_wfi(w)) {
                qemu_plugin_register_vcpu_insn_exec_inline(insn, QEMU_PLUGIN_INLINE_ADD_U64, &now.wfi, 1);
                if (track_burst) {
                    qemu_plugin_register_vcpu_insn_exec_cb(insn, on_wfi_burst, QEMU_PLUGIN_CB_NO_REGS, NULL);
                }
            }
        }
    }
}

static void on_atexit(qemu_plugin_id_t id, void *udata)
{
    uint64_t t;

    pthread_mutex_lock(&lock);
    stopping = true;
    pthread_cond_signal(&wake);
    pthread_mutex_unlock(&lock);
    pthread_join(sampler, NULL);

    t = mono_ns();
    if (win_started) {
        struct counters c = snapshot(t);
        emit("summary", &win, win_t_ns, &c, t);
    } else {
        fprintf(out, "{\"type\":\"summary\",\"started\":false,\"t_end_ms\":%.1f,\"start_ms\":%.1f}\n",
                t / 1e6, start_ns / 1e6);
    }
    fflush(out);
    if (out != stderr) {
        fclose(out);
    }
}

static bool parse_u64(const char *s, uint64_t *v)
{
    char *end;
    *v = strtoull(s, &end, 0);
    return *s && !*end;
}

static bool parse_bool(const char *s, bool *v)
{
    if (!strcmp(s, "on") || !strcmp(s, "1")) {
        *v = true;
    } else if (!strcmp(s, "off") || !strcmp(s, "0")) {
        *v = false;
    } else {
        return false;
    }
    return true;
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t *info,
                                           int argc, char **argv)
{
    const char *path = NULL;
    pthread_condattr_t ca;
    uint64_t v;

    t0_ns = 0;
    t0_ns = mono_ns();
    out = stderr;

    for (int i = 0; i < argc; i++) {
        const char *a = argv[i];
        if (!strncmp(a, "out=", 4)) {
            path = a + 4;
        } else if (!strncmp(a, "start_ms=", 9) && parse_u64(a + 9, &v)) {
            start_ns = v * 1000000u;
        } else if (!strncmp(a, "interval_ms=", 12) && parse_u64(a + 12, &v)) {
            interval_ns = v * 1000000u;
        } else if (!strncmp(a, "icount_shift=", 13) && parse_u64(a + 13, &v) && v < 32) {
            icount_shift = (int)v;
        } else if (!strncmp(a, "insns=", 6) && parse_bool(a + 6, &count_insns)) {
            continue;
        } else if (!strncmp(a, "burst=", 6) && parse_bool(a + 6, &track_burst)) {
            continue;
        } else {
            fprintf(stderr, "wakeups: bad option '%s'\n", a);
            return -1;
        }
    }
    if (track_burst && !count_insns) {
        fprintf(stderr, "wakeups: burst=on needs insns=on\n");
        return -1;
    }
    if (path && !(out = fopen(path, "w"))) {
        perror(path);
        return -1;
    }

    /* Deadlines are CLOCK_MONOTONIC, like mono_ns(), so host clock steps do not shift the window. */
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&wake, &ca);
    pthread_condattr_destroy(&ca);
    if (pthread_create(&sampler, NULL, sampler_main, NULL)) {
        fprintf(stderr, "wakeups: cannot start sampler thread\n");
        return -1;
    }

    qemu_plugin_register_vcpu_tb_trans_cb(id, on_tb_trans);
    qemu_plugin_register_vcpu_idle_cb(id, on_idle);
    qemu_plugin_register_vcpu_resume_cb(id, on_resume);
    qemu_plugin_register_atexit_cb(id, on_atexit, NULL);
    return 0;
}
