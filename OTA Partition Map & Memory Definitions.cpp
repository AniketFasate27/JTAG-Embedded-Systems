/* ota_jtag_validation.h */
#ifndef OTA_JTAG_VALIDATION_H
#define OTA_JTAG_VALIDATION_H

#include <stdint.h>
#include <stdbool.h>

/* ── Flash Partition Layout ───────────────────────────────────────────── */
#define FLASH_BASE_ADDR         0x08000000U   /* STM32 internal flash base  */
#define BOOTLOADER_START        0x08000000U
#define BOOTLOADER_SIZE         0x00008000U   /* 32 KB                      */

#define OTA_SLOT_A_START        0x08008000U   /* Active partition           */
#define OTA_SLOT_B_START        0x08040000U   /* Staging / candidate slot   */
#define OTA_SLOT_SIZE           0x00038000U   /* 224 KB each                */

#define OTA_METADATA_ADDR       0x08078000U   /* OTA metadata page          */
#define OTA_METADATA_SIZE       0x00001000U   /* 4 KB                       */

/* ── JTAG / CoreSight Debug Registers (ARM Cortex-M) ─────────────────── */
#define DHCSR_ADDR              0xE000EDF0U   /* Debug Halting Control      */
#define DHCSR_C_HALT            (1U << 1)
#define DHCSR_C_DEBUGEN         (1U << 0)
#define DHCSR_S_HALT            (1U << 17)
#define DHCSR_DBGKEY            (0xA05FU << 16)

#define DCRSR_ADDR              0xE000EDF4U   /* Debug Core Register Select */
#define DCRDR_ADDR              0xE000EDF8U   /* Debug Core Register Data   */
#define DEMCR_ADDR              0xE000EDFCU   /* Debug Exception/Monitor    */
#define DEMCR_VC_HARDERR        (1U << 10)
#define DEMCR_TRCENA            (1U << 24)

/* ── OTA Status Codes ─────────────────────────────────────────────────── */
typedef enum {
    OTA_STATE_IDLE          = 0x00,
    OTA_STATE_DOWNLOADING   = 0x01,
    OTA_STATE_VERIFYING     = 0x02,
    OTA_STATE_READY         = 0x03,
    OTA_STATE_APPLYING      = 0x04,
    OTA_STATE_SUCCESS       = 0x05,
    OTA_STATE_FAILED        = 0xFF
} ota_state_t;

/* ── OTA Metadata Structure ───────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint32_t  magic;              /* 0xDEADC0DE                            */
    uint32_t  version;            /* Semantic version: major.minor.patch   */
    uint32_t  image_size;         /* Firmware image size in bytes          */
    uint32_t  crc32;              /* CRC32 of the firmware image           */
    uint8_t   sha256[32];         /* SHA-256 hash of the image             */
    uint8_t   signature[64];      /* ECDSA-P256 signature                  */
    uint32_t  slot_active;        /* 0 = Slot A, 1 = Slot B               */
    uint8_t   state;              /* ota_state_t                           */
    uint8_t   retry_count;        /* Boot attempt counter                  */
    uint16_t  reserved;
    uint32_t  timestamp;          /* Unix timestamp of build               */
    uint32_t  metadata_crc;       /* CRC of this metadata struct itself    */
} ota_metadata_t;

/* ── JTAG Validation Result ───────────────────────────────────────────── */
typedef struct {
    bool   halt_success;
    bool   metadata_valid;
    bool   crc_valid;
    bool   hash_valid;
    bool   signature_valid;
    bool   boot_vector_sane;
    bool   no_hardfault_pending;
    bool   version_monotonic;
    uint32_t calculated_crc;
    uint32_t stored_crc;
    uint8_t  ota_state;
    uint32_t pc_at_halt;
    uint32_t sp_at_halt;
    uint32_t fault_status;       /* CFSR register                         */
} jtag_ota_validation_result_t;

#endif /* OTA_JTAG_VALIDATION_H */
