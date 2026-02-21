/* ota_jtag_validate.c */
#include "ota_jtag_validation.h"
#include <stdio.h>
#include <string.h>

#define OTA_MAGIC           0xDEADC0DEU
#define CFSR_ADDR           0xE000ED28U   /* Configurable Fault Status Reg */
#define SCB_VTOR            0xE000ED08U   /* Vector Table Offset Register  */

extern uint32_t  crc32_calculate(const uint8_t *, uint32_t);
extern void      sha256_compute (const uint8_t *, uint32_t, uint8_t *);
extern bool      jtag_halt_core (void);
extern void      jtag_resume_core(void);
extern uint32_t  jtag_read_core_register(uint8_t);
extern void      jtag_enable_debug_features(void);
extern void      jtag_read_flash_block(uint32_t, uint8_t *, uint32_t);

/* ── Step 1: Halt core and enable debug tracing ─────────────────────── */
static bool step_halt_and_enable_debug(jtag_ota_validation_result_t *res)
{
    jtag_enable_debug_features();
    res->halt_success = jtag_halt_core();
    if (!res->halt_success) {
        printf("[JTAG] ERROR: Core halt failed. Check JTAG connection.\n");
        return false;
    }
    /* Read PC (R15) and SP (R13) at halt point */
    res->pc_at_halt = jtag_read_core_register(15);
    res->sp_at_halt = jtag_read_core_register(13);
    printf("[JTAG] Core halted. PC=0x%08X  SP=0x%08X\n",
           res->pc_at_halt, res->sp_at_halt);
    return true;
}

/* ── Step 2: Validate OTA metadata integrity ────────────────────────── */
static bool step_validate_metadata(jtag_ota_validation_result_t *res,
                                   ota_metadata_t *meta)
{
    jtag_read_flash_block(OTA_METADATA_ADDR, (uint8_t *)meta,
                          sizeof(ota_metadata_t));

    /* Check magic number */
    if (meta->magic != OTA_MAGIC) {
        printf("[JTAG] FAIL: Bad magic 0x%08X (expected 0x%08X)\n",
               meta->magic, OTA_MAGIC);
        res->metadata_valid = false;
        return false;
    }

    /* Validate the metadata struct's own CRC */
    uint32_t meta_crc_check =
        crc32_calculate((const uint8_t *)meta,
                        sizeof(ota_metadata_t) - sizeof(uint32_t));

    if (meta_crc_check != meta->metadata_crc) {
        printf("[JTAG] FAIL: Metadata CRC mismatch. "
               "Got=0x%08X  Stored=0x%08X\n",
               meta_crc_check, meta->metadata_crc);
        res->metadata_valid = false;
        return false;
    }

    res->metadata_valid = true;
    res->ota_state      = meta->state;
    printf("[JTAG] Metadata valid. Version=0x%08X  State=0x%02X\n",
           meta->version, meta->state);
    return true;
}

/* ── Step 3: CRC32 check on the firmware image in staging slot ─────── */
static bool step_verify_firmware_crc(jtag_ota_validation_result_t *res,
                                     const ota_metadata_t *meta)
{
    if (meta->image_size == 0 || meta->image_size > OTA_SLOT_SIZE) {
        printf("[JTAG] FAIL: Invalid image size %u bytes\n", meta->image_size);
        res->crc_valid = false;
        return false;
    }

    /* Read firmware from the staging slot (Slot B) */
    static uint8_t fw_buf[OTA_SLOT_SIZE];   /* Static to avoid stack overflow */
    jtag_read_flash_block(OTA_SLOT_B_START, fw_buf, meta->image_size);

    uint32_t calc_crc = crc32_calculate(fw_buf, meta->image_size);
    res->calculated_crc = calc_crc;
    res->stored_crc     = meta->crc32;

    if (calc_crc != meta->crc32) {
        printf("[JTAG] FAIL: CRC32 mismatch. "
               "Calculated=0x%08X  Stored=0x%08X\n",
               calc_crc, meta->crc32);
        res->crc_valid = false;
        return false;
    }

    res->crc_valid = true;
    printf("[JTAG] CRC32 OK: 0x%08X\n", calc_crc);
    return true;
}

/* ── Step 4: SHA-256 hash check ─────────────────────────────────────── */
static bool step_verify_sha256(jtag_ota_validation_result_t *res,
                               const ota_metadata_t *meta)
{
    static uint8_t fw_buf[OTA_SLOT_SIZE];
    jtag_read_flash_block(OTA_SLOT_B_START, fw_buf, meta->image_size);

    uint8_t computed_hash[32];
    sha256_compute(fw_buf, meta->image_size, computed_hash);

    if (memcmp(computed_hash, meta->sha256, 32) != 0) {
        printf("[JTAG] FAIL: SHA-256 mismatch!\n");
        printf("  Computed: ");
        for (int i = 0; i < 32; i++) printf("%02X", computed_hash[i]);
        printf("\n  Stored:   ");
        for (int i = 0; i < 32; i++) printf("%02X", meta->sha256[i]);
        printf("\n");
        res->hash_valid = false;
        return false;
    }

    res->hash_valid = true;
    printf("[JTAG] SHA-256 OK.\n");
    return true;
}

/* ── Step 5: Verify reset vector and stack pointer sanity ────────────── */
static bool step_verify_boot_vector(jtag_ota_validation_result_t *res)
{
    /*
     * ARM Cortex-M vector table layout in Slot B:
     *   [0x00] = Initial Stack Pointer
     *   [0x04] = Reset Handler address (must be in Slot B range, odd for Thumb)
     */
    uint32_t initial_sp      = *((volatile uint32_t *)(OTA_SLOT_B_START));
    uint32_t reset_handler   = *((volatile uint32_t *)(OTA_SLOT_B_START + 4));

    /* Stack pointer must be in SRAM range (0x20000000–0x20080000 typical) */
    bool sp_sane = (initial_sp >= 0x20000000U && initial_sp <= 0x20080000U);

    /* Reset handler must be within Slot B and Thumb-encoded (LSB = 1) */
    bool rh_sane = ((reset_handler & 1U) == 1U) &&
                   ((reset_handler & ~1U) >= OTA_SLOT_B_START) &&
                   ((reset_handler & ~1U) <  OTA_SLOT_B_START + OTA_SLOT_SIZE);

    res->boot_vector_sane = sp_sane && rh_sane;

    printf("[JTAG] Boot vector: InitSP=0x%08X (%s)  ResetHdlr=0x%08X (%s)\n",
           initial_sp, sp_sane ? "OK" : "FAIL",
           reset_handler, rh_sane ? "OK" : "FAIL");

    return res->boot_vector_sane;
}

/* ── Step 6: Check for pending faults via CFSR ──────────────────────── */
static bool step_check_fault_status(jtag_ota_validation_result_t *res)
{
    uint32_t cfsr = *((volatile uint32_t *)CFSR_ADDR);
    res->fault_status = cfsr;

    if (cfsr != 0U) {
        printf("[JTAG] WARNING: CFSR=0x%08X — pending faults detected!\n", cfsr);
        if (cfsr & 0x0002U) printf("  → INVSTATE: Invalid execution state\n");
        if (cfsr & 0x0004U) printf("  → INVPC:    Invalid PC load\n");
        if (cfsr & 0x0008U) printf("  → NOCP:     No coprocessor\n");
        if (cfsr & 0x0100U) printf("  → IBUSERR:  Instruction bus error\n");
        if (cfsr & 0x8000U) printf("  → BFARVALID: Bus fault address valid\n");
        res->no_hardfault_pending = false;
        return false;
    }

    res->no_hardfault_pending = true;
    printf("[JTAG] Fault status: CLEAN (CFSR=0x00000000)\n");
    return true;
}

/* ── Step 7: Monotonic version check ────────────────────────────────── */
static bool step_check_version_monotonicity(jtag_ota_validation_result_t *res,
                                            const ota_metadata_t *meta)
{
    /* Read the active slot's metadata to get current running version */
    ota_metadata_t active_meta;
    jtag_read_flash_block(OTA_SLOT_A_START, (uint8_t *)&active_meta,
                          sizeof(ota_metadata_t));

    if (active_meta.magic != OTA_MAGIC) {
        /* No valid active firmware – first flash, allow any version */
        res->version_monotonic = true;
        printf("[JTAG] Version check: No active firmware, skipping.\n");
        return true;
    }

    if (meta->version > active_meta.version) {
        res->version_monotonic = true;
        printf("[JTAG] Version: 0x%08X → 0x%08X (upgrade OK)\n",
               active_meta.version, meta->version);
    } else {
        res->version_monotonic = false;
        printf("[JTAG] FAIL: Downgrade attempt! "
               "Active=0x%08X  Candidate=0x%08X\n",
               active_meta.version, meta->version);
    }
    return res->version_monotonic;
}

/* ── Master Validation Routine ──────────────────────────────────────── */
jtag_ota_validation_result_t jtag_validate_ota_update(void)
{
    jtag_ota_validation_result_t result;
    memset(&result, 0, sizeof(result));

    ota_metadata_t meta;
    memset(&meta, 0, sizeof(meta));

    printf("\n========================================\n");
    printf("   JTAG OTA Validation Starting...\n");
    printf("========================================\n");

    /* Run all validation steps in order; abort on critical failure */
    if (!step_halt_and_enable_debug(&result))   goto done;
    if (!step_validate_metadata(&result, &meta)) goto resume;
                step_verify_firmware_crc(&result, &meta);
                step_verify_sha256(&result, &meta);
                step_verify_boot_vector(&result);
                step_check_fault_status(&result);
                step_check_version_monotonicity(&result, &meta);

resume:
    jtag_resume_core();
    printf("[JTAG] Core resumed.\n");

done:
    /* ── Print Summary Report ──────────────────────────────────────── */
    printf("\n========================================\n");
    printf("   JTAG OTA Validation Report\n");
    printf("========================================\n");
    printf("  Core Halt          : %s\n", result.halt_success         ? "PASS" : "FAIL");
    printf("  Metadata Valid     : %s\n", result.metadata_valid       ? "PASS" : "FAIL");
    printf("  CRC32 Match        : %s  (0x%08X)\n",
           result.crc_valid ? "PASS" : "FAIL", result.calculated_crc);
    printf("  SHA-256 Match      : %s\n", result.hash_valid           ? "PASS" : "FAIL");
    printf("  Boot Vector Sane   : %s\n", result.boot_vector_sane     ? "PASS" : "FAIL");
    printf("  No Pending Faults  : %s  (CFSR=0x%08X)\n",
           result.no_hardfault_pending ? "PASS" : "FAIL", result.fault_status);
    printf("  Version Monotonic  : %s\n", result.version_monotonic    ? "PASS" : "FAIL");
    printf("  OTA State          : 0x%02X\n", result.ota_state);

    bool all_pass = result.halt_success      &&
                    result.metadata_valid    &&
                    result.crc_valid         &&
                    result.hash_valid        &&
                    result.boot_vector_sane  &&
                    result.no_hardfault_pending &&
                    result.version_monotonic;

    printf("----------------------------------------\n");
    printf("  OVERALL: %s\n", all_pass ? "✅ PASS" : "❌ FAIL");
    printf("========================================\n\n");

    return result;
}
