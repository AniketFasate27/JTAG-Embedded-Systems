/* jtag_access.c */
#include "ota_jtag_validation.h"
#include <string.h>
#include <stdio.h>

/*
 * NOTE: These register R/W functions abstract the underlying JTAG transport.
 * Replace the body with your probe SDK calls:
 *   - OpenOCD  → use TCL commands via pipe or libOpenOCD
 *   - SEGGER J-Link → JLINKARM_ReadMemU32 / WriteU32
 *   - PyOCD   → target.read32 / write32
 *   - CMSIS-DAP → DAP_Transfer
 */

/* ── Memory-mapped register access (when running on-target) ────────────── */
static inline uint32_t reg_read32(uint32_t addr)
{
    return *((volatile uint32_t *)addr);
}

static inline void reg_write32(uint32_t addr, uint32_t val)
{
    *((volatile uint32_t *)addr) = val;
}

/* ── Halt the core via DHCSR ─────────────────────────────────────────── */
bool jtag_halt_core(void)
{
    reg_write32(DHCSR_ADDR, DHCSR_DBGKEY | DHCSR_C_DEBUGEN | DHCSR_C_HALT);

    /* Poll S_HALT bit with timeout (100 ms equivalent in loops) */
    for (volatile uint32_t i = 0; i < 1000000U; i++) {
        if (reg_read32(DHCSR_ADDR) & DHCSR_S_HALT) {
            return true;
        }
    }
    return false;   /* Timeout – core did not halt */
}

/* ── Resume the core ─────────────────────────────────────────────────── */
void jtag_resume_core(void)
{
    uint32_t dhcsr = reg_read32(DHCSR_ADDR);
    dhcsr &= ~DHCSR_C_HALT;
    reg_write32(DHCSR_ADDR, DHCSR_DBGKEY | dhcsr);
}

/* ── Read a CPU core register (R0–R15, PSR) via DCRSR/DCRDR ─────────── */
uint32_t jtag_read_core_register(uint8_t reg_id)
{
    /* Write register ID to DCRSR with REGWnR = 0 (read) */
    reg_write32(DCRSR_ADDR, (uint32_t)reg_id & 0x1FU);

    /* Wait for S_REGRDY */
    while (!(reg_read32(DHCSR_ADDR) & (1U << 16))) { /* S_REGRDY bit 16 */ }

    return reg_read32(DCRDR_ADDR);
}

/* ── Enable DEMCR trace / fault catching ─────────────────────────────── */
void jtag_enable_debug_features(void)
{
    uint32_t demcr = reg_read32(DEMCR_ADDR);
    demcr |= DEMCR_TRCENA | DEMCR_VC_HARDERR;   /* Catch hard faults      */
    reg_write32(DEMCR_ADDR, demcr);
}

/* ── Read a block of flash memory ────────────────────────────────────── */
void jtag_read_flash_block(uint32_t addr, uint8_t *buf, uint32_t len)
{
    /* For on-target execution this is a direct memcpy from flash.
       For host-side JTAG probes, replace with probe SDK memory read. */
    memcpy(buf, (const void *)addr, len);
}
