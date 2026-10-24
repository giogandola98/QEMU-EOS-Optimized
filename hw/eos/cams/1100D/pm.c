/*
 * Canon EOS 1100D 1.0.5: power management (PowerMgr idle loop and selfRefresh deep sleep;
 * ML-Optimized docs/firmware/1100D-powersave.md). Selected by eos_model_desc.pm_handler
 * (cams/1100D/model.inc.c); see hw/eos/pm.h.
 * Without these, Canon's mode-2 idle loop finds neither sleep condition and spins at 0xFF1EA818.
 *   0xC0400008 bit1  image-engine clock (LCLK): written by ClockSaver, stored by eos_handle_basic
 *   0xC0F01004       LCLK client mask: reads 0x400, what the low-clock WFI path expects at idle
 *   0xC0400004       clock select (0x03 <-> 0x17 around that WFI): stored
 *   0xC0220094 bit0  selfRefresh enabled (GPIO): 1
 *   0xC0210600 bit0, 0xC0100110 bit31, 0xC0100204   handshakes: ready at once. 0xC022F198 keeps its
 *                    0/1 toggle (eos_handle_gpio): the bootloader waits for bit0 = 1 there, selfRefresh for 0
 *   0xC0201308 bit4  wake flag polled with IRQs masked: set while an interrupt is pending. While none is,
 *                    the vCPU is halted (like WFI) instead of spinning, until the next interrupt.
 *   0xC0400008 = 0   selfRefresh gates every module clock, timers included: from that write until Canon
 *                    restores a non-zero value the DIGIC timers, the DryOS tick and the HPTimers stand
 *                    still (eos_interrupt_timer_body), so only a key/MPU interrupt ends the sleep, as on the
 *                    camera. Each sleep logs "[PM1100D] self-refresh #n in" and, on wake, its length in
 *                    virtual time and the interrupt that ended it.
 * QEMU_EOS_PM_TRACE=1 prints the first accesses to each of these registers and every LCLK edge;
 * QEMU_EOS_PM_MODEL=0 disables
 * the model.
 */

#include "qemu/osdep.h"
#include "qemu/timer.h"
#include "hw/core/cpu.h"
#include "hw/eos/eos.h"
#include "hw/eos/pm.h"
#include "profile.h"

bool eos_pm_1100d(unsigned int address, unsigned char type, unsigned int value, unsigned int *ret)
{
    static uint32_t clksel, lclk_mask, wake_ctrl, timer600 = 1, basic204;
    static unsigned int sr_count, wake_irq;
    static int64_t sr_start_ns, sr_total_ns;
    static int trace = -1;
    static unsigned int trace_count[8];
    uint32_t *reg = NULL;
    int idx;

    switch (address) {
    case 0xC0F01004: idx = 0; reg = &lclk_mask; break;
    case 0xC0400004: idx = 1; reg = &clksel; break;
    case 0xC0220094: idx = 2; break;
    case 0xC0210600: idx = 3; reg = &timer600; break;
    case 0xC0100204: idx = 4; reg = &basic204; break;
    case 0xC022F198: idx = 5; break;
    case 0xC0201308: idx = 6; reg = &wake_ctrl; break;
    case 0xC0400008: idx = 7; break;        /* clock gate; the value is stored by eos_handle_basic */
    default:
        return false;
    }

    bool handled = true;
    if (type & MODE_WRITE) {
        if (reg) {
            *reg = value;
        }
        if (idx == 7 && !value != eos_pm_clocks_gated()) {
            int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            eos_pm_set_clocks_gated(!value);
            if (!value) {
                sr_start_ns = now;
                wake_irq = 0;
                fprintf(stderr, "[PM1100D] self-refresh #%u in (pc %08X)\n", ++sr_count,
                        CURRENT_CPU->env.regs[15]);
            } else {
                sr_total_ns += now - sr_start_ns;
                fprintf(stderr, "[PM1100D] self-refresh #%u out after %.3f s, wake irq 0x%02X, total %.3f s\n",
                        sr_count, (now - sr_start_ns) / 1e9, wake_irq, sr_total_ns / 1e9);
            }
        }
        /* timer 6 control, 0xC022F198 and CLOCK_ENABLE keep their usual handlers */
        handled = (idx != 3 && idx != 5 && idx != 7);
    } else {
        switch (idx) {
        case 0: *ret = 0x400; break;
        case 2: *ret = 1; break;
        case 5: handled = false; break;
        case 6: {
            CPUState *cs = CPU(CURRENT_CPU);
            bool pending = eos_state->irq_id || (cs->interrupt_request & CPU_INTERRUPT_HARD);
            *ret = (wake_ctrl & ~0x10u) | (pending ? 0x10 : 0);
            if (pending && !wake_irq) {
                wake_irq = eos_state->irq_id;
            }
            if (!pending) {
                /* takes effect at the end of this TB: the poll loop sleeps until an interrupt */
                cpu_interrupt(cs, CPU_INTERRUPT_HALT);
            }
            break;
        }
        case 7: handled = false; break;
        default: *ret = *reg; break;
        }
    }

    if (trace < 0) {
        const char *env = getenv("QEMU_EOS_PM_TRACE");
        trace = env && env[0] == '1';
    }
    if (trace && idx == 7 && (type & MODE_WRITE) && ((value ^ eos_state->clock_enable) & 2)) {
        /* every LCLK edge (ClockSaver), not only the first 40 accesses: what the deep sleep depends on */
        fprintf(stderr, "[PM1100D] LCLK %s (CLOCK_ENABLE 0x%X, pc %08X)\n", (value & 2) ? "on" : "off",
                value, CURRENT_CPU->env.regs[15]);
    }
    if (trace && idx == 0 && (type & MODE_WRITE)) {
        /* ClockSaver client mask: who changes it (return addresses on the stack), to find the client
         * keeping LCLK on */
        uint32_t sp = CURRENT_CPU->env.regs[13], w[40];
        cpu_physical_memory_read(sp, w, sizeof(w));
        fprintf(stderr, "[PM1100D] LCLK mask <- 0x%X lr %08X, ROM words on stack:", value,
                CURRENT_CPU->env.regs[14]);
        for (int i = 0; i < 40; i++) {
            if (w[i] >= 0xFF000000) {
                fprintf(stderr, " %08X", w[i]);
            }
        }
        fprintf(stderr, "\n");
    }
    if (trace && trace_count[idx]++ < 40) {
        fprintf(stderr, "[PM1100D] %s 0x%08X %s 0x%X (pc %08X)\n", (type & MODE_WRITE) ? "W" : "R",
                address, (type & MODE_WRITE) ? "<-" : "->", (type & MODE_WRITE) ? value : (handled ? *ret : 0),
                CURRENT_CPU->env.regs[15]);
    }
    return handled;
}
