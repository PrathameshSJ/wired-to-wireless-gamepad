# Full flash backup, 2026-11-15

Taken just before the rename to "PSJ CUSTOM". The pad was then named
"PSJ GamePad Custom", in Trusted mode, with 2 saved devices
(a laptop and a phone).

| File | What |
|---|---|
| `flash_full.bin` | the whole 4 MB flash: bootloader, partition table, firmware, NVS (calibration + Bluetooth pairing keys) |
| `flash_full.bin.sha256` | checksum |
| `get_bonds.txt` | `GET` / `BONDS` output at backup time (git-ignored: device addresses) |
| `quantum_pad.ino` | the firmware source that was running |

`flash_full.bin` holds the pairing keys, so it's git-ignored. Keep it private.
It was checked against the chip (`verify-flash`, digest matched).

## Restore (puts back everything exactly, including pairings)

```sh
ET=$(ls -d ~/.arduino15/packages/esp32/tools/esptool_py/*/esptool | head -1)
sha256sum -c backups/2026-11-15_full-flash/flash_full.bin.sha256
$ET --chip esp32c3 -p /dev/ttyACM0 -b 921600 write-flash 0 backups/2026-11-15_full-flash/flash_full.bin
```
