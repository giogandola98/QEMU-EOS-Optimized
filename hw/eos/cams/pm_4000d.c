/*
 * Canon EOS 4000D 1.1.0: power management (PowerMgr idle loop and selfRefresh deep sleep).
 * Selected by eos_model_desc.pm_handler (model_list.c); see hw/eos/pm.h and docs/models/4000D.md.
 *
 * PowerMgr task (ROM 0xFE289544), mode 2 ("power save", Canon's counter at 0):
 *   0xC0400008 bit1 set (image-engine clock LCLK on): if the LCLK client mask 0xC0F01004 == 0x400,
 *     a low-clock WFI bracketed by writes to 0xC0400044/48 (5, then 0) and 0xC0400030 bit0. Already
 *     reachable without a model: 0xC0F01000-FF is plain storage and holds what ClockSaver writes.
 *   0xC0400008 bit1 clear, > 1000 ticks since the last stamp: "[PM] pmSelfRefresh : In", selfRefresh
 *     (0xFE0C02E4 via the thunk 0xFE0C9618), "Out".
 * selfRefresh, only if GPIO 0xC0220018 bit0 is set: IRQ/FIQ masked, D-cache cleaned and off, handshakes
 *   on 0xC0100204/0x300/0x724 and a poll of 0xC0100110 bit31, saves CLOCK_ENABLE and writes 0, clears
 *   clock select 0xC0400004 bits 0-1 and 0xC04000B4, then **WFI with interrupts masked**: the CPU stays
 *   there until an interrupt is asserted (no wake-flag poll, unlike the 1100D's 0xC0201308). On the way
 *   out it restores 0xC04000B4 = 3, clock select |= 3, CLOCK_ENABLE |= 1, runs a handshake on
 *   0xC022F17C/198/19C/50x (polls 0xC022F198 bit0), writes the saved CLOCK_ENABLE back, undoes the
 *   0xC0100xxx handshakes, D-cache on, IRQs restored (the pending interrupt is taken there).
 *
 * The existing handlers already answer the polls: 0xC0220018 reads 1 (GPIO default), 0xC0100110 reads
 * 0x80000000 (eos_handle_basic, from the 1300D), 0xC022F198 toggles 0/1 (eos_handle_gpio), and the
 * ARM946 WFI (mcr p15,0,r0,c15,c8,2) halts the vCPU until an interrupt is pending even with CPSR I/F set.
 * What this model adds is the clock gate: from Canon's CLOCK_ENABLE = 0 until its next non-zero write
 * the DIGIC timers, the DryOS tick and the HPTimers stand still (eos_pm_set_clocks_gated), so the WFI
 * ends only with a key/MPU interrupt, as on the camera. Without it the DryOS tick ended every sleep
 * within 10 ms and PowerMgr re-entered selfRefresh about once a second.
 * Each sleep logs "[PM4000D] self-refresh #n in" and, on wake, its length in virtual time and the
 * interrupt that ended it. QEMU_EOS_PM_TRACE=1 also prints every LCLK edge and the first accesses to
 * the PM registers.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "hw/eos/eos.h"
#include "hw/eos/pm.h"

#define PM4000D_CLOCK_ENABLE    0xC0400008      /* bit1 = LCLK; 0 = all module clocks off (selfRefresh) */

static const uint32_t pm4000d_traced[] = {
    PM4000D_CLOCK_ENABLE,
    0xC0F01004,     /* LCLK client mask (ClockSaver); low-clock WFI needs 0x400 */
    0xC0400004,     /* clock select */
    0xC0400030, 0xC0400044, 0xC0400048,     /* low-clock WFI bracket */
    0xC04000B4,     /* cleared before the selfRefresh WFI, 3 after */
    0xC0220018,     /* selfRefresh enabled (GPIO bit0) */
    0xC0100110,     /* ready bit31 polled before the clock gate */
    0xC022F198,     /* handshake polled after the wake */
};

static void pm4000d_trace(unsigned int address, unsigned char type, unsigned int value)
{
    static int trace = -1;
    static unsigned int count[ARRAY_SIZE(pm4000d_traced)];

    if (trace < 0) {
        const char *env = getenv("QEMU_EOS_PM_TRACE");
        trace = env && env[0] == '1';
    }
    if (!trace) {
        return;
    }
    if (address == PM4000D_CLOCK_ENABLE && (type & MODE_WRITE) && ((value ^ eos_state->clock_enable) & 2)) {
        /* every LCLK edge (ClockSaver): what the deep sleep depends on */
        fprintf(stderr, "[PM4000D] LCLK %s (CLOCK_ENABLE 0x%X, pc %08X)\n", (value & 2) ? "on" : "off",
                value, CURRENT_CPU->env.regs[15]);
    }
    for (int i = 0; i < ARRAY_SIZE(pm4000d_traced); i++) {
        if (pm4000d_traced[i] == address && count[i]++ < 40) {
            /* reads are logged before the handler answers, so only writes carry a value */
            if (type & MODE_WRITE) {
                fprintf(stderr, "[PM4000D] W 0x%08X <- 0x%X (pc %08X)\n", address, value,
                        CURRENT_CPU->env.regs[15]);
            } else {
                fprintf(stderr, "[PM4000D] R 0x%08X (pc %08X)\n", address, CURRENT_CPU->env.regs[15]);
            }
        }
    }
}

bool eos_pm_4000d(unsigned int address, unsigned char type, unsigned int value, unsigned int *ret)
{
    static unsigned int sr_count;
    static int64_t sr_start_ns, sr_total_ns;

    pm4000d_trace(address, type, value);

    if (address != PM4000D_CLOCK_ENABLE || !(type & MODE_WRITE)) {
        return false;
    }

    /* CLOCK_ENABLE write: the value itself is stored by eos_handle_basic */
    bool gate = (value == 0);
    if (gate != eos_pm_clocks_gated()) {
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
        eos_pm_set_clocks_gated(gate);
        if (gate) {
            sr_start_ns = now;
            fprintf(stderr, "[PM4000D] self-refresh #%u in (pc %08X)\n", ++sr_count,
                    CURRENT_CPU->env.regs[15]);
        } else {
            /* first write after the WFI (CLOCK_ENABLE |= 1): the interrupt that woke it is still
             * pending, IRQs are masked until selfRefresh returns */
            sr_total_ns += now - sr_start_ns;
            fprintf(stderr, "[PM4000D] self-refresh #%u out after %.3f s, wake irq 0x%02X, total %.3f s\n",
                    sr_count, (now - sr_start_ns) / 1e9, eos_state->irq_id, sr_total_ns / 1e9);
        }
    }
    return false;
}
