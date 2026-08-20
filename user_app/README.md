# Application-Driven Dual-Bank OTA Updates

The bootloader supports an optional application-driven dual-bank OTA upgrade mechanism (`dual_bank_ota.h`).

## Overview

- **Bank 0 (`DFU_BANK_0_REGION_START`)**: Active application slot (e.g. `0x1000` or `0x26000` with SoftDevice).
- **Bank 1 (`DFU_BANK_1_REGION_START`)**: OTA staging slot.
- **Trailer (`DUAL_BANK_TRAILER_ADDR`)**: Placed at the end of Bank 1.
- **Partition Geometry**: Dynamically calculated from the bootloader's existing `dfu_types.h` engine (respecting MCU flash size, SoftDevice presence, and `DFU_APP_DATA_RESERVED`).

When an application receives an OTA update (e.g. via Zigbee, BLE, ESP32 co-processor, or cellular modem), it writes the incoming binary into Bank 1 and writes a signed `dual_bank_trailer_t` at the end of Bank 1. On the next reboot, if `DUALBANK_FW=1` is enabled:
1. Bootloader validates the trailer magic (`OTAP`), structure CRC32, target address, and size bounds (preventing trailer overlap).
2. Bootloader validates the Cortex-M vector table in Bank 1 (initial SP in valid SRAM, Reset Handler in Bank 0 space with Thumb bit set).
3. Bootloader validates the whole image CRC32.
4. Bootloader erases Bank 0 and safely copies Bank 1 -> Bank 0 while petting any active hardware watchdog timer.
5. Bootloader validates Bank 0 flash contents, synchronizes bootloader settings (bypassing stale serial DFU CRC16), and flips trailer magic to `OTAA` (`DUAL_BANK_MAGIC_APPLIED`). If flash verification fails, it invalidates the trailer to prevent infinite write wear loops.

## Enabling in the Bootloader Build

Dual-bank firmware support is disabled by default to preserve maximum flash size for single-bank applications. To enable it:

### Make:
```bash
make BOARD=feather_nrf52840_express DUALBANK_FW=1 all
```

### CMake:
```bash
cmake -S . -B cmake-build-feather_nrf52840_express -DBOARD=feather_nrf52840_express -DDUALBANK_FW=ON
cmake --build cmake-build-feather_nrf52840_express
```

## Application Integration Example

```c
#include "dual_bank_ota.h"

void finalize_ota_update(const uint8_t *new_fw_data, size_t fw_size, uint32_t version_code)
{
    /* 1. Calculate image CRC32 */
    uint32_t image_crc = dual_bank_crc32(new_fw_data, fw_size);

    /* 2. Initialize trailer struct */
    dual_bank_trailer_t trailer;
    dual_bank_trailer_init(&trailer, fw_size, image_crc, version_code);

    /* 3. Write trailer to the end of Bank 1 flash */
    flash_write(DUAL_BANK_TRAILER_ADDR, &trailer, sizeof(trailer));

    /* 4. Reboot into bootloader to apply update */
    NVIC_SystemReset();
}
```
