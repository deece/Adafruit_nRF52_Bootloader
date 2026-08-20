/*
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Alastair D'Silva
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef DUAL_BANK_OTA_H_
#define DUAL_BANK_OTA_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Magic Signatures */
#define DUAL_BANK_MAGIC_PENDING     0x4F544150UL  /* "OTAP" - OTA Update Pending Activation */
#define DUAL_BANK_MAGIC_APPLIED     0x4F544141UL  /* "OTAA" - OTA Update Successfully Applied */
#define DUAL_BANK_MAGIC_FAILED      0x4F544146UL  /* "OTAF" - OTA Update Flash Verification Failed */
#define DUAL_BANK_VERSION_1         1UL

/* Flash Layout Definitions */
#ifdef DFU_TYPES_H__
#define DUAL_BANK_PAGE_SIZE         CODE_PAGE_SIZE
#define DUAL_BANK_BANK0_ADDR        DFU_BANK_0_REGION_START
#define DUAL_BANK_BANK0_SIZE        DFU_IMAGE_MAX_SIZE_BANKED
#define DUAL_BANK_BANK1_ADDR        DFU_BANK_1_REGION_START
#define DUAL_BANK_BANK1_SIZE        DFU_IMAGE_MAX_SIZE_BANKED
#else
/* Default standalone layout for nRF52840 (MBR-only, 40KB reserved app data) */
#ifndef DUAL_BANK_PAGE_SIZE
#define DUAL_BANK_PAGE_SIZE         4096UL
#endif

#ifndef DUAL_BANK_BANK0_ADDR
#define DUAL_BANK_BANK0_ADDR        0x00001000UL  /* Active Application Slot (Bank 0) */
#endif

#ifndef DUAL_BANK_BANK0_SIZE
#define DUAL_BANK_BANK0_SIZE        0x00074000UL  /* 464 KB (116 pages) */
#endif

#ifndef DUAL_BANK_BANK1_ADDR
#define DUAL_BANK_BANK1_ADDR        0x00075000UL  /* Staging / OTA Slot (Bank 1) */
#endif

#ifndef DUAL_BANK_BANK1_SIZE
#define DUAL_BANK_BANK1_SIZE        0x00074000UL  /* 464 KB (116 pages) */
#endif
#endif

/**
 * @brief Dual-Bank Update Trailer structure.
 * Stored at the very end of Bank 1 flash space.
 */
typedef struct __attribute__((packed)) {
    uint32_t magic;         /**< DUAL_BANK_MAGIC_PENDING or DUAL_BANK_MAGIC_APPLIED */
    uint32_t struct_ver;    /**< DUAL_BANK_VERSION_1 */
    uint32_t image_size;    /**< Size of firmware binary in Bank 1 in bytes */
    uint32_t image_crc32;   /**< IEEE 802.3 CRC-32 of firmware binary payload */
    uint32_t target_addr;   /**< Destination flash address (must match DUAL_BANK_BANK0_ADDR) */
    uint32_t version_code;  /**< Monotonic version code / build timestamp */
    uint32_t flags;         /**< Flags (reserved for future use, default 0) */
    uint32_t trailer_crc32; /**< CRC-32 of all preceding fields in this structure */
} dual_bank_trailer_t;

#define DUAL_BANK_TRAILER_ADDR      (DUAL_BANK_BANK1_ADDR + DUAL_BANK_BANK1_SIZE - sizeof(dual_bank_trailer_t))

/**
 * @brief Calculate standard IEEE 802.3 CRC-32 (polynomial 0xEDB88320).
 */
static inline uint32_t dual_bank_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFUL;
    if (data == NULL) return 0;

    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320UL;
            } else {
                crc = (crc >> 1);
            }
        }
    }
    return ~crc;
}

/**
 * @brief Helper to initialize and sign a dual_bank_trailer_t struct.
 */
static inline void dual_bank_trailer_init(dual_bank_trailer_t *trailer,
                                         uint32_t image_size,
                                         uint32_t image_crc32,
                                         uint32_t version_code)
{
    if (!trailer) return;
    trailer->magic        = DUAL_BANK_MAGIC_PENDING;
    trailer->struct_ver   = DUAL_BANK_VERSION_1;
    trailer->image_size   = image_size;
    trailer->image_crc32  = image_crc32;
    trailer->target_addr  = DUAL_BANK_BANK0_ADDR;
    trailer->version_code = version_code;
    trailer->flags        = 0;
    trailer->trailer_crc32 = dual_bank_crc32((const uint8_t *)trailer,
                                            offsetof(dual_bank_trailer_t, trailer_crc32));
}

#ifdef __cplusplus
}
#endif

#endif /* DUAL_BANK_OTA_H_ */
