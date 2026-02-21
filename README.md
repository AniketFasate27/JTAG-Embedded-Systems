# JTAG-Embedded-Systems
# 🔍 JTAG OTA Firmware Validation Framework

> A hardware-assisted OTA (Over-The-Air) firmware validation framework for ARM Cortex-M
> microcontrollers, leveraging JTAG debug access to perform cryptographic integrity checks,
> fault inspection, and anti-rollback enforcement — all before a single instruction of the
> candidate firmware executes.

![Language](https://img.shields.io/badge/Language-C99-blue?style=flat-square&logo=c)
![Platform](https://img.shields.io/badge/Platform-ARM%20Cortex--M-brightgreen?style=flat-square&logo=arm)
![JTAG](https://img.shields.io/badge/Debug-JTAG%20%2F%20SWD-orange?style=flat-square)
![Standard](https://img.shields.io/badge/Medical-IEC%2062304%20Class%20C-red?style=flat-square)
![License](https://img.shields.io/badge/License-MIT-lightgrey?style=flat-square)

---

## 📋 Table of Contents

- [Overview](#overview)
- [Why JTAG-Based Validation?](#why-jtag-based-validation)
- [System Architecture](#system-architecture)
- [Flash Partition Layout](#flash-partition-layout)
- [Validation Pipeline](#validation-pipeline)
- [File Structure](#file-structure)
- [Key Data Structures](#key-data-structures)
- [Register Reference](#register-reference)
- [Building & Running](#building--running)
- [Integration Guide](#integration-guide)
- [IEC 62304 Traceability Notes](#iec-62304-traceability-notes)
- [Limitations & Future Work](#limitations--future-work)

---

## Overview

Over-the-Air firmware updates are a critical capability in modern embedded systems — but they
also represent one of the highest-risk operations a device can perform. A corrupted or
maliciously crafted update can permanently brick a device or, in safety-critical applications,
endanger lives.

This framework uses the JTAG/SWD debug interface to **halt the CPU** immediately after an OTA
image lands in the staging flash partition, and performs a comprehensive 7-step validation
sequence before the bootloader is permitted to apply the update:

| Step | Check | Mechanism |
|------|-------|-----------|
| 1 | Core halt & debug enable | `DHCSR` write + `S_HALT` poll |
| 2 | OTA metadata integrity | Magic number + struct CRC32 |
| 3 | Firmware image CRC32 | Full Slot B scan |
| 4 | SHA-256 hash match | FIPS 180-4 digest comparison |
| 5 | Boot vector sanity | Initial SP + Reset Handler address check |
| 6 | Pending fault detection | `CFSR` register inspection |
| 7 | Anti-rollback enforcement | Monotonic version comparison |

---

## Why JTAG-Based Validation?

Traditional OTA validation runs **on the target itself** — meaning it shares the same CPU,
flash controller, and potential attack surface as the firmware being validated. JTAG-based
validation is architecturally separate:
```
┌────────────────────┐         JTAG/SWD         ┌────────────────────────┐
│   Host / CI Runner │ ◄──────────────────────► │   Target MCU (Halted)  │
│                    │                           │                        │
│  - Hash checks     │     DAP / CoreSight       │  - Flash read via DRW  │
│  - CRC checks      │◄──── Memory Access ──────►│  - Register read via   │
│  - Fault checks    │                           │    DCRSR/DCRDR         │
│  - Audit logging   │                           │  - CFSR fault status   │
└────────────────────┘                           └────────────────────────┘
```

**Key advantages:**
- Validation logic cannot be tampered with by a compromised firmware image
- CPU is halted — no race conditions during flash inspection
- Full register state (PC, SP) is captured at the time of validation
- Compatible with CI/CD pipelines — returns exit code `0` (pass) or `1` (fail)
- Audit trail captured at the hardware level, suitable for IEC 62304 Class C evidence

---

## System Architecture
```
ota_jtag_validation/
│
├── Validation Orchestrator       ← jtag_validate_ota_update()
│   │
│   ├── Step 1: JTAG Halt         ← DHCSR, DEMCR setup
│   ├── Step 2: Metadata Check    ← OTA_METADATA_ADDR (0x08078000)
│   ├── Step 3: CRC32 Verify      ← Slot B full image scan
│   ├── Step 4: SHA-256 Verify    ← FIPS 180-4 digest
│   ├── Step 5: Boot Vector       ← Vector table @ Slot B base
│   ├── Step 6: Fault Status      ← CFSR @ 0xE000ED28
│   ├── Step 7: Version Check     ← Anti-rollback monotonic compare
│   └── Resume Core               ← DHCSR C_HALT clear
│
├── JTAG Access Layer             ← jtag_access.c
│   ├── jtag_halt_core()
│   ├── jtag_resume_core()
│   ├── jtag_read_core_register()
│   ├── jtag_enable_debug_features()
│   └── jtag_read_flash_block()
│
└── Crypto Utilities              ← crypto_verify.c
    ├── crc32_calculate()
    └── sha256_compute()
```

---

## Flash Partition Layout
```
STM32 Internal Flash — 0x08000000 to 0x0807FFFF (512 KB)

 0x08000000 ┌──────────────────────────────┐
            │        Bootloader            │  32 KB
 0x08008000 ├──────────────────────────────┤
            │                              │
            │       Slot A (Active)        │  224 KB  ← Currently running firmware
            │                              │
 0x08040000 ├──────────────────────────────┤
            │                              │
            │       Slot B (Staging)       │  224 KB  ← OTA candidate lives here
            │                              │
 0x08078000 ├──────────────────────────────┤
            │       OTA Metadata           │   4 KB   ← Validated by Step 2
 0x08079000 └──────────────────────────────┘
```

> **Note:** Partition addresses are configurable via `#define` in `ota_jtag_validation.h`.
> Adjust to match your linker script.

---

## Validation Pipeline

### Step 1 — Core Halt & Debug Enable

The `DHCSR` (Debug Halting Control and Status Register) is written with the `DBGKEY` unlock
sequence and `C_HALT` bit. The framework polls `S_HALT` for up to ~100ms. On success, the
current `PC` (R15) and `SP` (R13) are captured via `DCRSR`/`DCRDR` and stored in the result
struct for audit logging.

`DEMCR` is also configured with `VC_HARDERR` to vector-catch hard faults and `TRCENA` to
enable the trace subsystem.

---

### Step 2 — OTA Metadata Integrity

The `ota_metadata_t` struct at `OTA_METADATA_ADDR` is read and validated in two sub-checks:

1. **Magic number** — Must equal `0xDEADC0DE`. Any other value indicates uninitialized or
   corrupted metadata.
2. **Metadata CRC** — A CRC32 is computed over all fields *except* the trailing
   `metadata_crc` field itself. This detects bit-flips or partial writes to the metadata page.

The struct also carries the OTA state machine value (`ota_state_t`), retry counter, build
timestamp, and ECDSA-P256 signature field (signature verification hookpoint — plug in your
PKI stack here).

---

### Step 3 — Firmware Image CRC32

The full firmware image in **Slot B** is read via JTAG memory access and a CRC32 (IEEE 802.3
polynomial `0xEDB88320`) is computed. The result is compared against `meta->crc32`.

This detects transmission-layer errors: flash write failures, partial downloads, ECC
corrections that changed data silently.

---

### Step 4 — SHA-256 Hash Verification

A FIPS 180-4 compliant SHA-256 digest is computed over the same Slot B image and compared
against `meta->sha256[32]`. Unlike CRC32, SHA-256 is cryptographically collision-resistant
and detects intentional tampering.

> In production, replace the bundled software SHA-256 with your MCU's hardware crypto
> accelerator (e.g., STM32 HASH peripheral, nRF52 CryptoCell) for performance and
> side-channel resistance.

---

### Step 5 — Boot Vector Sanity

The ARM Cortex-M vector table begins at the base of Slot B. Two fields are inspected:

- **`[0x0000]` — Initial Stack Pointer**: Must fall within SRAM bounds
  (`0x20000000`–`0x20080000`). An out-of-range value indicates a blank or mis-linked image.
- **`[0x0004]` — Reset Handler Address**: Must be Thumb-encoded (LSB = `1`) and point within
  the Slot B address range. A reset handler pointing outside the slot would cause an
  immediate fault on boot.

---

### Step 6 — Pending Fault Detection

The `CFSR` (Configurable Fault Status Register) at `0xE000ED28` aggregates UsageFault,
BusFault, and MemManage fault status bits. Any non-zero value indicates a fault occurred
during or before the OTA process — this is a red flag that the system may be in an undefined
state. Individual fault bits are decoded and printed to the log.

---

### Step 7 — Monotonic Version Check

The active firmware metadata in **Slot A** is read and its version field compared against the
candidate in Slot B. The candidate version must be **strictly greater than** the active
version. Downgrades are rejected to enforce anti-rollback protection.

If Slot A contains no valid firmware (magic mismatch — first flash scenario), the check is
skipped and the candidate is permitted.

---

## File Structure
```
ota_jtag_validation/
├── include/
│   └── ota_jtag_validation.h   # Addresses, structs, enums, result type
├── src/
│   ├── jtag_access.c           # Low-level JTAG register R/W, halt/resume
│   ├── crypto_verify.c         # CRC32 and SHA-256 implementations
│   ├── ota_jtag_validate.c     # 7-step validation orchestrator
│   └── main_test.c             # Test entry point (CI/CD compatible)
├── CMakeLists.txt
└── README.md
```

---

## Key Data Structures

### `ota_metadata_t` — Stored at `0x08078000`

| Field | Type | Description |
|-------|------|-------------|
| `magic` | `uint32_t` | Must be `0xDEADC0DE` |
| `version` | `uint32_t` | Encoded as `major.minor.patch` |
| `image_size` | `uint32_t` | Firmware image byte count |
| `crc32` | `uint32_t` | CRC32 of firmware image |
| `sha256[32]` | `uint8_t[]` | SHA-256 digest of firmware image |
| `signature[64]` | `uint8_t[]` | ECDSA-P256 signature (hookpoint) |
| `slot_active` | `uint32_t` | `0` = Slot A, `1` = Slot B |
| `state` | `uint8_t` | `ota_state_t` value |
| `retry_count` | `uint8_t` | Boot attempt counter |
| `timestamp` | `uint32_t` | Unix build timestamp |
| `metadata_crc` | `uint32_t` | CRC32 of all preceding fields |

### `jtag_ota_validation_result_t` — Returned by `jtag_validate_ota_update()`

| Field | Type | Description |
|-------|------|-------------|
| `halt_success` | `bool` | Core halted successfully |
| `metadata_valid` | `bool` | Magic + metadata CRC passed |
| `crc_valid` | `bool` | Image CRC32 matched |
| `hash_valid` | `bool` | SHA-256 matched |
| `boot_vector_sane` | `bool` | SP and Reset Handler valid |
| `no_hardfault_pending` | `bool` | CFSR was zero |
| `version_monotonic` | `bool` | No downgrade detected |
| `pc_at_halt` | `uint32_t` | Program Counter at halt |
| `sp_at_halt` | `uint32_t` | Stack Pointer at halt |
| `fault_status` | `uint32_t` | Raw CFSR value |
| `calculated_crc` | `uint32_t` | CRC32 computed by validator |
| `ota_state` | `uint8_t` | State read from metadata |

---

## Register Reference

| Register | Address | Usage in this framework |
|----------|---------|------------------------|
| `DHCSR` | `0xE000EDF0` | Halt / resume core, poll S_HALT |
| `DCRSR` | `0xE000EDF4` | Select core register to read |
| `DCRDR` | `0xE000EDF8` | Read selected core register value |
| `DEMCR` | `0xE000EDFC` | Enable trace, vector-catch hard faults |
| `CFSR` | `0xE000ED28` | UsageFault / BusFault / MemManage status |
| `SCB_VTOR` | `0xE000ED08` | Vector Table Offset Register |

> All addresses are ARM Cortex-M CoreSight standard — valid across STM32, nRF52, LPC,
> SAMD, and other Cortex-M0/M3/M4/M7/M33 parts.

---

## Building & Running

### Prerequisites

- `arm-none-eabi-gcc` (GCC 10+)
- CMake 3.16+
- A JTAG/SWD probe: J-Link, ST-Link v2, CMSIS-DAP, or OpenOCD-compatible

### Build
```bash
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../arm-none-eabi.cmake
make -j4
```

### On-target execution (via GDB + OpenOCD)
```bash
# Terminal 1 — Start OpenOCD
openocd -f interface/jlink.cfg -f target/stm32f4x.cfg

# Terminal 2 — Flash and run validator
arm-none-eabi-gdb build/ota_jtag_validation.elf
(gdb) target remote :3333
(gdb) load
(gdb) monitor reset halt
(gdb) continue
```

### CI/CD integration

The `main_test.c` entry point returns exit code `0` on full pass and `1` on any failure,
making it directly usable in GitHub Actions, Jenkins, or any shell-based pipeline:
```yaml
# .github/workflows/ota_validate.yml
- name: Run JTAG OTA Validation
  run: |
    ./scripts/flash_and_validate.sh
    echo "Exit code: $?"
```

---

## Integration Guide

### Swapping the JTAG transport layer

The `jtag_access.c` functions (`reg_read32`, `reg_write32`, `jtag_read_flash_block`) are
deliberately thin wrappers. Replace their bodies with your probe SDK:

| Probe SDK | Read call | Write call |
|-----------|-----------|------------|
| SEGGER J-Link | `JLINKARM_ReadMemU32()` | `JLINKARM_WriteU32()` |
| PyOCD (Python) | `target.read32()` | `target.write32()` |
| OpenOCD TCL | `mdw` / `read_memory` | `mww` / `write_memory` |
| CMSIS-DAP | `DAP_Transfer()` | `DAP_Transfer()` |

### Adding ECDSA signature verification

The `signature[64]` field in `ota_metadata_t` is a reserved hookpoint. Plug in your PKI
stack in `step_validate_metadata()` after the metadata CRC check:
```c
/* Add after metadata CRC check in step_validate_metadata() */
if (!ecdsa_p256_verify(meta->sha256, meta->signature, PUBLIC_KEY)) {
    printf("[JTAG] FAIL: Signature verification failed\n");
    res->signature_valid = false;
    return false;
}
res->signature_valid = true;
```

Recommended libraries: **mbedTLS**, **wolfSSL**, or **micro-ecc** for constrained targets.

---

## IEC 62304 Traceability Notes

This framework was designed with **IEC 62304 Class C** medical device software requirements
in mind. Each validation step maps to a software unit verification activity:

| IEC 62304 Clause | Validation Step | Evidence Captured |
|------------------|-----------------|-------------------|
| 5.5.1 — Unit verification | Steps 3 & 4 | CRC32 + SHA-256 pass/fail with values |
| 5.5.2 — Integration testing | Step 5 | Boot vector PC/SP values logged |
| 5.7.2 — System testing | Steps 6 & 7 | CFSR value, version numbers logged |
| 7.1.3 — Anomaly review | Step 6 | Full CFSR bit decode printed |
| 9.8 — Problem resolution | All steps | `jtag_ota_validation_result_t` serializable to audit log |

> The `pc_at_halt` and `sp_at_halt` fields give you a precise hardware-level snapshot of
> execution state at the moment of validation — valuable evidence in a design history file
> (DHF) or device master record (DMR).

---

## Limitations & Future Work

- **ECDSA signature verification** is stubbed — integration with a full PKI stack is
  required for production use.
- **SHA-256** uses a software implementation. For production, use the MCU's hardware crypto
  peripheral to prevent timing side-channels.
- **Slot B buffer** (`static uint8_t fw_buf[OTA_SLOT_SIZE]`) uses 224 KB of static RAM.
  For memory-constrained targets, switch to a streaming CRC/hash approach.
- **Anti-rollback** currently uses a simple integer version compare. A secure monotonic
  counter backed by OTP fuses or a Trusted Execution Environment (TEE) is recommended for
  high-security applications.
- Host-side Python validation runner (using PyOCD) planned for a future release.

---

## License

MIT License — see [`LICENSE`](LICENSE) for details.

---

<div align="center">
  <sub>Built for safety-critical embedded systems · ARM Cortex-M · JTAG/SWD</sub>
</div>
