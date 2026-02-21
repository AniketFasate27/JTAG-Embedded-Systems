/* main_test.c */
#include "ota_jtag_validation.h"
#include <stdio.h>

int main(void)
{
    jtag_ota_validation_result_t result = jtag_validate_ota_update();

    /* Return 0 on full pass, 1 on any failure (useful for CI/CD pipelines) */
    bool all_pass = result.halt_success      &&
                    result.metadata_valid    &&
                    result.crc_valid         &&
                    result.hash_valid        &&
                    result.boot_vector_sane  &&
                    result.no_hardfault_pending &&
                    result.version_monotonic;

    return all_pass ? 0 : 1;
}
```

---

## Architecture Summary
```
┌─────────────────────────────────────────────────────────────────┐
│                    JTAG OTA Validation Flow                     │
├─────────────────────────────────────────────────────────────────┤
│  1. JTAG Halt Core        → DHCSR write, S_HALT poll           │
│  2. Metadata Validation   → Magic + struct CRC check           │
│  3. CRC32 Verification    → Full image CRC from Slot B          │
│  4. SHA-256 Verification  → Cryptographic hash check           │
│  5. Boot Vector Check     → Initial SP + Reset Handler sanity  │
│  6. Fault Status Check    → CFSR register inspection           │
│  7. Version Monotonicity  → Anti-rollback protection           │
│  8. Resume Core           → DHCSR C_HALT clear                 │
└─────────────────────────────────────────────────────────────────┘
