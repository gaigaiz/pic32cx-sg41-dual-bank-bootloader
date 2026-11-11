# PIC32CX-SG41 Dual-Bank Fail-Safe Bootloader

A UART bootloader with hardware dual-bank (A/B) OTA, APP CRC verification,
version tracking, and watchdog (WDT) supervised rollback for the
**PIC32CX1025SG41128** (Cortex-M4, 1 MB Flash, 256 KB RAM).

## 1. What this project solves

Updating MCU firmware over a serial port can brick the device if power is cut
mid-write or the new image crashes on boot. This project keeps two independent
Flash banks: one active bank runs the current firmware, the other bank acts as
the OTA staging buffer. A new APP is written to the inactive bank and CRC-checked
before a hardware bank swap; if the new APP keeps crashing, the watchdog
supervisor rolls back automatically and, if both banks fail, leaves the device
in bootloader upgrade mode.

## 2. Main features

- **MCC Melody UART bootloader** over SERCOM4, 115200 8N1 (TX PB12 / PAD0, RX PB13 / PAD1).
- **Hardware dual bank** via `NVMCTRL_BankSwap()` (BKSWRST); the active/inactive
  roles of the two physical banks are swapped by hardware after reset.
- **APP region CRC verification** using the DSU CRC32 before jumping to APP.
- **Version tracking**: the BL records the APP version; a successful OTA bumps
  the version by one, and the BL prints `APP Vx.0` on entry.
- **WDT supervised rollback**:
  - The BL disables the WDT before jumping to APP; the APP enables the WDT and
    feeds it itself.
  - After the APP stabilizes it writes a success marker that clears the retry
    counter.
  - Only a **WDT timeout reset** increments the retry counter; POR and
    NVIC software resets do not count.
  - After 3 consecutive WDT failures the BL performs a BankSwap once; if the
    swapped bank also fails 3 times, the BL stays in upgrade mode (both banks dead).
- **Return to bootloader**: hold SW1 (PD00) for 3 seconds; the APP writes a RAM
  trigger magic and resets, and the BL detects the magic and stays in upgrade mode.

## 3. Hardware and Flash layout

### Physical partition (fixed addresses, unchanged by swap)

| Region | Start | End | Size |
|---|---:|---:|---:|
| BL_A | `0x00000000` | `0x00001FFF` | `0x2000` (8 KB) |
| APP_A | `0x00002000` | `0x0007FFFF` | `0x7E000` (504 KB) |
| BL_B | `0x00080000` | `0x00081FFF` | `0x2000` (8 KB) |
| APP_B | `0x00082000` | `0x000FFFFF` | `0x7E000` (504 KB) |

Flash page = 512 bytes, erase block = 8192 bytes.

### State sector (active-bank mapped address `0x7E000`, 8 KB block)

| Offset | Field |
|---:|---|
| `0x00` | Expected APP CRC32 (`0xFFFFFFFF` = not recorded) |
| `0x04` | APP version number (`0xFFFFFFFF` = treated as 0) |
| `0x08` | WDT retry counter (`0xFFFFFFFF` = treated as 0) |
| `0x0C` | Bank-swapped flag (`0` = not swapped, `1` = swapped once) |

### Pins

| Name | Pin | Direction / active level |
|---|---|---|
| SW1 | PD00 | Input, internal pull-up, active low |
| LED1 | PC21 | Output, active low |
| LED2 | PA16 | Output, active low |

## 4. Directory structure

```
.
├── BL/                       BL bootloader project
│   ├── .vscode/BL.mplab.json BL toolchain / linker options
│   └── config.mcc/src/
│       ├── main.c
│       ├── custom_trigger.c   Firmware trigger override (must be kept)
│       └── config/default/
│           ├── initialization.c
│           ├── bootloader/
│           │   ├── bootloader_common.h
│           │   ├── bootloader_common.c
│           │   └── bootloader_uart.c
│           └── peripheral/nvmctrl/plib_nvmctrl.c
├── APP/                      APP application project
│   ├── .vscode/APP.mplab.json APP linker macros / heap / optimization
│   └── config.mcc/src/main.c
├── Tools/
│   ├── btl_host.py           Official host tool (raw binary only)
│   ├── make_merged_bin.py    Build bl.bin / app.bin / merged_bl_app.bin from HEX
│   ├── verify_merged_bin.py  Independent byte/CRC check of the generated bins
│   └── inspect_hex.py        Inspect HEX record types and address ranges
└── docs/                     Usage manual and change notes
```

## 5. Build prerequisites

- **Toolchain**: XC32 v5.10, MPLAB VSCode extension (bundled CMake + Ninja).
- **Debugger**: on-board PKOB4.
- **Python 3** for the host tools and image scripts.
- After generating code with MCC, restore `APP.mplab.json` `ROM_LENGTH` to
  `0x7C000` so the state sector is protected (MCC generate resets it to `0xFE000`).

## 6. Usage

### 6.1 Build images

1. Build the BL project in VSCode/MPLAB, producing `BL/out/BL/default.hex`.
2. Build the APP project, producing `APP/out/APP/default.hex`.
3. Generate the raw binaries:

```
python Tools/make_merged_bin.py
```

4. Independently verify the generated images:

```
python Tools/verify_merged_bin.py
```

The check passes when the script prints `ALL CHECKS PASSED`.

### 6.2 Serial upgrade

`btl_host.py` accepts **raw binary only** (it does not parse Intel HEX).

APP-only OTA to the inactive bank:

```
python Tools/btl_host.py -v -s -i COM3 -d PIC32CX_SG41 -a 0x82000 -f BL/out/BL/app.bin
```

Program a blank inactive bank with both BL and APP (merged image):

```
python Tools/btl_host.py -v -s -i COM3 -d PIC32CX_SG41 -a 0x80000 -f BL/out/BL/merged_bl_app.bin
```

- `-a 0x82000` = inactive bank APP area (always points to the inactive bank).
- `-a 0x80000` = inactive bank BL base, for the merged BL+APP image.
- `-s` = send `0xA4` BankSwap Reset after CRC passes; without it the script sends
  a plain `0xA3` reset.

### 6.3 Serial protocol

Frame: guard `0x5048434D` (little-endian `4D 43 48 50`), 4-byte size, 1-byte
command, payload. No frame-tail CRC; the whole-region CRC is passed in VERIFY.

| Command | Name | Payload | Action |
|---:|---|---|---|
| `0xA0` | UNLOCK | begin addr 4B LE + length 4B LE | Unlock the erase/write range |
| `0xA1` | DATA | addr 4B LE + data block (8192 B) | Erase block and write Flash |
| `0xA2` | VERIFY | CRC32 4B LE | DSU CRC32 compare over the unlocked range |
| `0xA3` | RESET | none | Plain `NVIC_SystemReset()`, no swap |
| `0xA4` | BKSWAP_RESET | none | Reply OK, then `NVMCTRL_BankSwap()` |
| `0xA5` | DEVCFG_DATA | config-bit data | Fuse programming disabled in this project |
| `0xA6` | READ_VERSION | none | Reply `50` then major, minor version |

| Reply | Meaning |
|---:|---|
| `0x50` | OK, command accepted |
| `0x51` | ERROR, illegal UNLOCK address / length |
| `0x52` | INVALID, unknown command code |
| `0x53` | CRC_OK, VERIFY passed |
| `0x54` | CRC_FAIL, VERIFY failed |

### 6.4 Examples

Read BL version (size = 0; pad the command code with four `00` bytes):

```
Send:   4D 43 48 50 00 00 00 00 A6 00 00 00 00
Reply:  50 03 07
```

Unknown command:

```
Send:   4D 43 48 50 00 00 00 00 AB 00 00 00 00
Reply:  52
```

Illegal UNLOCK address:

```
Send:   4D 43 48 50 08 00 00 00 A0 00 20 08 00 00 00 00 00
Reply:  51
```

## 7. WDT rollback behavior (single active bank, inactive bank empty)

| WDT resets | Behavior |
|---:|---|
| 1st / 2nd | retry_cnt = 1 / 2, jump back to current bank APP |
| 3rd | retry_cnt = 3, BankSwap to the other bank, retry_cnt cleared |
| After swap | If the other bank APP area is empty, BL detects empty Flash and stays in upgrade mode |

## 8. Notes

- The BL does not broadcast a `READY` banner on its own; send READ_VERSION to
  confirm the BL is in upgrade mode.
- A size-0 command sent as a bare 9-byte frame may get no reply; pad after the
  command code with four `00` bytes (the host tool does this automatically).
- After the OTA log reports `Verification success` / `Reboot Done`, do not rely
  on those strings to judge the active bank; read `NVMCTRL_STATUS.AFIRST` in the
  debugger memory window to confirm which bank is active.

## 9. Documentation

- `docs/Bootloader双Bank使用手册.md` — full usage manual (Chinese).
- `docs/BL工程改造为Fail-Safe双Bank_修改说明.md` — BL dual-bank change notes (Chinese).

## 10. License

Released under the [MIT License](LICENSE).
