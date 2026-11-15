# Quantum Pad

A dead Quantum Hi-Tech USB gamepad converted into a Bluetooth LE controller with an
ESP32-C3 SuperMini. It shows up as **"PSJ CUSTOM"** but uses the Xbox Series X|S
IDs and report layout (`045E:0B13`), so Android, Windows, iOS and Linux treat it as an
Xbox controller with no setup.

## Hardware

| Part | Notes |
|---|---|
| ESP32-C3 SuperMini | the brain |
| 2× 74HC165 (daisy-chained) | 16 buttons. **HC, not LS**: 74LS165 is 5 V TTL and reads every button as pressed at 3.3 V |
| 16× 10 kΩ pull-downs | buttons are active-high: button common goes to 3.3 V |
| TP4056 + LiPo | charging. **Battery switch OFF while USB is plugged in** |

### Pins

| GPIO | Use |
|---|---|
| 0 / 1 | left stick X / Y |
| 4 / 3 | right stick X / Y |
| 5 | 74HC165 LOAD |
| 6 | 74HC165 CLK |
| 7 | DATA (chip 1 QH) |
| 20 | Mode button, wired straight to GND (internal pull-up) |
| 10 | optional strong pull-up for a leaky Mode pad (220 Ω to GPIO20); unused now |

Avoid GPIO 2, 8 and 9 (strapping pins): a stick on GPIO2 once forced download-mode boot.
Stick pots must be fed from 3.3 V, never 5 V.

Shift chain: chip 2 QH → chip 1 SER, chip 1 QH → GPIO7.
Chip 1 A–H = Up, Down, Left, Right, Select, Start, L3, R3.
Chip 2 A–H = △, ○, ✕, □, L1, R1, L2, R2.
3D board layout: open `tools/perfboard_10x9.html` in a browser.

## Folders

| Path | What |
|---|---|
| `quantum_pad/` | the firmware |
| `tools/live.py` + `live.html` | live input tester and calibration wizard |
| `tools/perfboard_10x9.html` | 3D perfboard / wiring diagram (`perfboard_old.html` = earlier layout) |
| `tools/mon.py`, `tools/record.py` | serial capture / record button activity |
| `diagnostics/` | bench sketches: shift-register diag, pin wiggle for the multimeter, stick test |
| `backups/` | earlier firmware + saved calibrations, each with notes |

## Build and flash

Needs `arduino-cli` with the `esp32:esp32` core, plus the libraries
**ESP32-BLE-Gamepad 0.8.0** and **NimBLE-Arduino 2.5.1**.

```sh
sudo setfacl -m u:$USER:rw /dev/ttyACM0      # if you're not in the uucp group
FQBN=esp32:esp32:esp32c3:CDCOnBoot=cdc
arduino-cli compile --fqbn $FQBN quantum_pad
arduino-cli upload  --fqbn $FQBN -p /dev/ttyACM0 quantum_pad
```

Flashing keeps the calibration: it lives in NVS flash and survives reflashing as long
as `CFG_VER` in the firmware doesn't change.

## Calibrating

```sh
python3 tools/live.py        # then open http://localhost:8765
```

Run the wizard over **USB** (battery OFF): it maps every button, measures the stick
ranges and centres, sets the deadzone and saves it all onto the pad. Without USB the page
still shows live input over Bluetooth, but it can't save.

The serial commands behind it (`GET`, `MAP`, `CAL`, `DZ`, `INV`, `MODEPOL`, `SWAP`,
`PROBE`, `SAVE`, `DEFAULTS`) are documented at the top of `quantum_pad/quantum_pad.ino`.

## Behaviour

- **Buttons:** Xbox layout by position (south = A, east = B, west = X, north = Y),
  L1/R1 = bumpers, L2/R2 = full-press triggers, Select = View, Start = Menu.
- **Xbox/Guide button:** Mode, or hold L3 + R3 for 1 second.
- **Stuck inputs:** any input that reads pressed at power-on is ignored until it has
  been released once.
- **Disabling a broken channel:** set bits in `DISABLED_RAW` in the firmware
  (bit n = wizard bar n+1). `MODE_DISABLED` does the same for Mode. Both are off now.

## Bluetooth

### Link modes

| Mode | How | What it does | Blue LED |
|---|---|---|---|
| **Trusted** | every power-on | Hidden from scan lists. Only saved devices can connect; anything else is dropped and not saved | short blip every 2 s while waiting, off when connected |
| **Pairing** | hold **L1 + R1 + Mode for 3 s** | Drops the current device, advertises from a private address made with a fresh key each time (saved devices can't recognise it, so they don't keep retrying), shows up in scan lists (and Windows' Swift Pair popup), saves whatever pairs | fast blink, slower once something has paired |

| **Wired** | power on from a computer's USB, or the combo from Pairing while on USB | Bluetooth off. Input goes over the USB cable through `tools/usb_pad.py` (see below) | steady on |

The combo (**L1 + R1 + Mode, 3 s**) steps through the modes:
- **on USB:** Wired → Trusted → Pairing → Wired …
- **on battery:** Trusted ↔ Pairing

Power-on picks the start mode: plugged into a computer = **Wired**, otherwise **Trusted**
(a phone charger counts as battery). Plugging USB in while the pad is already on changes
nothing, so you can top up mid-game and stay on Bluetooth. Pulling the cable while
**Wired** drops to **Trusted** after 1.5 s.

- Hold the combo again to go back to **Trusted**, now with the new device trusted. Devices
  that pair learn the pad's real address during pairing, so they reconnect to it afterwards.
- To re-pair a device the pad has forgotten, remove the pad on that device first, or it
  keeps trying the old keys.
- Pairing mode switches back to Trusted by itself after 3 minutes with nothing connected.
- The pad saves up to **3 devices**; pairing a 4th replaces the oldest.
- It connects to one device at a time.
- Serial commands: `PAIR`, `TRUST`, `BONDS` (list saved devices), `FORGET` (delete them all).
- **Windows** shows a "connect?" popup (Swift Pair) while the pad is in pairing mode.
- **Android's** earbud-style popup (Google Fast Pair) needs a model registered with Google,
  so it isn't implemented.
- The device name (`PAD_NAME` in the firmware) travels in the scan response: it doesn't fit
  in the 31-byte advertising packet next to the other fields, and without a name Android hides the pad from its
  pairing list.
- After changing the name or IDs, remove and re-pair the pad on Linux. If the old name
  sticks, fix `Name=` in `/var/lib/bluetooth/<adapter>/cache/<pad MAC>` and restart
  `bluetooth`.

### Wired mode (`tools/usb_pad.py`)

The C3's USB port can only be a serial port, never a USB gamepad (that needs an ESP32-S2/S3).
In Wired mode the pad streams `G <buttons> <lx> <ly> <rx> <ry> <lt> <rt>` lines over
serial, and the helper turns them into a virtual **Xbox 360 pad** (uinput, Linux only):

```sh
python3 tools/usb_pad.py              # --bt-off: force Wired on connect, --invert-y: flip sticks
```

While it runs, type `o` + Enter for Bluetooth off (Wired), `t` for Trusted, `p` for
Pairing, `s` for status. Needs python-evdev and the `input` group. Don't run it together
with `tools/live.py` (same serial port).

## Calibration in the firmware

The wizard calibration from 2026-11-14 is also the firmware's built-in default
(`DEFAULT_MAP`, `DEFAULT_MIN/CTR/MAX`, `DEFAULT_SWAP`), so `DEFAULTS` or a wiped flash still
gives a working pad. After recalibrating, copy the new `GET` values there too.

## To do

- D-pad Up doesn't work: no input is mapped to it, and raw input 12 (wizard bar 13) is
  unassigned. Check that button's wire to chip 2 pin 14, then rerun the wizard.


## Credits

Firmware, tools and docs written with the help of [Claude](https://claude.com/claude-code) (Anthropic) via Claude Code.
