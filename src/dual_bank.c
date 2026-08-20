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

#include "dual_bank.h"
#include "bootloader_types.h"
#include "dfu_types.h"
#include "flash_nrf5x.h"
#include "boards.h"
#include "nrf_wdt.h"
#include "nrfx_nvmc.h"

#if defined(DUALBANK_FW) && (DUALBANK_FW == 1)

static uint32_t __attribute__((noinline)) dual_bank_crc32_calc(const uint8_t *data, size_t len) {
  uint32_t crc = 0xFFFFFFFFUL;
  if (data == NULL) {
    return 0;
  }
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint32_t)data[i];
    for (uint8_t j = 0; j < 8; j++) {
      crc = (crc >> 1) ^ (0xEDB88320UL & -(crc & 1));
    }
  }
  return ~crc;
}

static void dual_bank_wdt_feed(void) {
  if (NRF_WDT->RUNSTATUS) {
    for (int i = 0; i < 8; i++) {
      NRF_WDT->RR[i] = 0x6E524635UL;
    }
  }
}

static void dual_bank_update_trailer(uint32_t trailer_addr, uint32_t page_size,
                                     const dual_bank_trailer_t *orig_trailer, uint32_t new_magic) {
  dual_bank_trailer_t updated_trailer = *orig_trailer;
  updated_trailer.magic = new_magic;
  updated_trailer.trailer_crc32 =
      dual_bank_crc32_calc((const uint8_t *)&updated_trailer, offsetof(dual_bank_trailer_t, trailer_crc32));

  uint32_t trailer_page_addr = trailer_addr & ~(page_size - 1);
  nrfx_nvmc_page_erase(trailer_page_addr);
  nrfx_nvmc_words_write(trailer_addr, (const uint32_t *)&updated_trailer, sizeof(updated_trailer) / 4);
}

void dual_bank_check_and_apply_update(void) {
  const uint32_t bank0_addr = DFU_BANK_0_REGION_START;
  const uint32_t bank_size = DFU_IMAGE_MAX_SIZE_BANKED;
  const uint32_t bank1_addr = DFU_BANK_1_REGION_START;
  const uint32_t page_size = CODE_PAGE_SIZE;
  const uint32_t trailer_addr = bank1_addr + bank_size - sizeof(dual_bank_trailer_t);
  const uint32_t max_image_size = bank_size - sizeof(dual_bank_trailer_t);

  const dual_bank_trailer_t *trailer = (const dual_bank_trailer_t *)trailer_addr;

  /* Fast check: Magic signature, version, self-CRC, size bounds, and target address */
  if (trailer->magic != DUAL_BANK_MAGIC_PENDING ||
      trailer->struct_ver != DUAL_BANK_VERSION_1 ||
      trailer->image_size == 0 || trailer->image_size > max_image_size ||
      trailer->target_addr != bank0_addr ||
      trailer->trailer_crc32 !=
          dual_bank_crc32_calc((const uint8_t *)trailer, offsetof(dual_bank_trailer_t, trailer_crc32))) {
    return;
  }

  /* Verify Cortex-M Vector Table in Bank 1 */
  const uint32_t *vectors = (const uint32_t *)bank1_addr;
  uint32_t initial_sp = vectors[0];
  uint32_t reset_handler = vectors[1];

  /* Initial SP must be in valid SRAM range for this chip */
  uint32_t const ram_start = 0x20000000UL;
  uint32_t const ram_end = ram_start + (NRF_FICR->INFO.RAM << 10u);
  if (initial_sp < ram_start || initial_sp > ram_end || (initial_sp & 3U) != 0) {
    return;
  }

  /* Reset handler must point within Bank 0 address range with Thumb bit set */
  if ((reset_handler & 1) == 0 || (reset_handler & ~1UL) < bank0_addr ||
      (reset_handler & ~1UL) >= (bank0_addr + trailer->image_size)) {
    return;
  }

  /* Feed watchdog before long operations */
  dual_bank_wdt_feed();

  /* Verify Bank 1 payload CRC32 */
  if (dual_bank_crc32_calc((const uint8_t *)bank1_addr, trailer->image_size) != trailer->image_crc32) {
    return;
  }

  led_state(STATE_WRITING_STARTED);
  dual_bank_wdt_feed();

  /* Erase Bank 0 pages needed */
  flash_nrf5x_erase(bank0_addr, trailer->image_size);
  dual_bank_wdt_feed();

  /* Copy Bank 1 -> Bank 0 */
  flash_nrf5x_write(bank0_addr, (const void *)bank1_addr, trailer->image_size, false);
  flash_nrf5x_flush(false);
  dual_bank_wdt_feed();

  /* Verify Bank 0 flash contents */
  if (dual_bank_crc32_calc((const uint8_t *)bank0_addr, trailer->image_size) == trailer->image_crc32) {
    /* Mark trailer as applied */
    dual_bank_update_trailer(trailer_addr, page_size, trailer, DUAL_BANK_MAGIC_APPLIED);

    /* Sync bootloader settings page so bootloader_app_is_valid() validates the new app without stale DFU CRC16 */
    bootloader_settings_t settings = {0};
    settings.bank_0 = BANK_VALID_APP;
    settings.bank_0_crc = 0; /* 0 indicates CRC16 checking is bypassed in bootloader_app_is_valid */
    settings.bank_0_size = trailer->image_size;
    settings.bank_1 = BANK_INVALID_APP;

    nrfx_nvmc_page_erase(BOOTLOADER_SETTINGS_ADDRESS);
    nrfx_nvmc_words_write(BOOTLOADER_SETTINGS_ADDRESS, (const uint32_t *)&settings, sizeof(settings) / 4);
  } else {
    /* Invalidate trailer magic to 0 (clearing bits without erase) to prevent rewrite loops */
    nrfx_nvmc_word_write(trailer_addr, 0x00000000UL);
  }

  led_state(STATE_WRITING_FINISHED);
  dual_bank_wdt_feed();
}

#endif /* DUALBANK_FW */
