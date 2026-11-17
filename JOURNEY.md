# Build Journal

How one dead Quantum-branded USB gamepad became **PSJ CUSTOM**, a Bluetooth controller
that phones and PCs see as an Xbox controller. For the general how-to, see the
[README](README.md).

## 1. The donor (16 October 2026)

The pad had stopped working over USB. Like most cheap pads, all of its logic sits in one
epoxy-covered chip on the main board, so there was nothing to repair. The plan was to
keep the shell, buttons and sticks and replace the electronics.

![Original controller board: epoxy chip, cable pads and passives](media/photos/01_original-board-epoxy-chip.jpg)
*The original board. The black blob is the controller chip; the pads at the top
(`G`, `V`, `-`, `+`) are where the USB cable was soldered.*

![All the parts laid out](media/photos/02_parts-laid-out.jpg)
*The parts: the pad's main button board, shoulder-button boards and stick board, plus
the new electronics: ESP32-C3 SuperMini (in the bag), TP4056 USB-C charger, 1000 mAh LiPo,
perfboard and a slide switch. The rumble motor was kept but is not used.*

![Button board and stick board on the bench](media/photos/03_button-board-and-stick-board.jpg)
*Mapping the button board with a multimeter, stick board in the foreground.*

## 2. Tapping the buttons (17 October)

Each button trace was cut away from the old chip and given its own wire, and the
commons were separated from the old circuit.

![Wires soldered to the left button pads](media/photos/04_wires-on-button-pads-left.jpg)
*Wires on the D-pad side.*

![Wires soldered to the right button pads](media/photos/05_wires-on-button-pads-right.jpg)
*Wires on the face-button side.*

![Button board fully wired](media/photos/06_button-board-wired.jpg)
*The main board with its wires attached.*

![Stick board wired](media/photos/07_stick-board-wired.jpg)
*The stick board with its wires attached.*

![Back of the button board](media/photos/08_button-board-back.jpg)
*The back of the main board.*

![Bench overview](media/photos/09_bench-overview.jpg)
*Test-fitting the boards back into the shell.*

![Board back in the shell](media/photos/10_board-back-in-shell.jpg)
*Main board seated in the front shell with its wires routed out.*

![Wired board with the new modules](media/photos/11_wired-board-and-modules.jpg)
*The wired main board next to the new modules.*

## 3. The shift-register board (17 October)

Sixteen buttons are too many for the SuperMini's free pins, so two daisy-chained
shift registers read them over three wires.

![Shift-register board, top](media/photos/12_shift-register-board-top.jpg)
*First attempt with GD74LS165 chips. This was a mistake: the LS family is 5 V TTL logic
and, powered from 3.3 V, read every button as pressed. They were replaced with 74HC165s.*

![Shift-register board, bottom](media/photos/13_shift-register-board-bottom.jpg)
*The solder side, with the two decoupling capacitors.*

## 4. Assembly (8 November)

Everything was wired together and packed into the shell. An interactive 3D diagram of
the perfboard ([`tools/perfboard_10x9.html`](tools/perfboard_10x9.html)) was used to
check pin numbers while soldering.

![Final assembly wiring](media/photos/14_final-assembly-wiring.jpg)
*The finished wiring before closing: perfboard with the pull-down resistors, SuperMini,
charger and battery.*

![Final assembly wiring, second angle](media/photos/15_final-assembly-wiring-2.jpg)
*The same, from the other side.*

Videos:

- [Assembly in the shell](media/videos/01_assembly-in-shell.mp4) (8 s)
- [Wiring, with the 3D perfboard diagram on screen](media/videos/02_wiring-and-3d-diagram.mp4) (27 s)
- [Testing every input in the live tester](media/videos/03_testing-all-inputs.mp4) (64 s)

## 5. Firmware and debugging (8 to 15 November)

The first firmware presented the pad as a generic Bluetooth gamepad. Getting from there
to a reliable controller took several rounds:

- **Download-mode boots.** A stick on GPIO2 (a strapping pin) made the board boot into
  the flasher. The sticks moved to GPIO 0, 1, 3 and 4.
- **Xbox identity.** Switching to the Xbox Series X|S Bluetooth layout made Android,
  Windows and Linux map the buttons correctly with no setup.
- **Invisible on phones.** The name did not fit in the 31-byte advertising packet, and
  Android hides nameless devices. The name moved to the scan response.
- **Broken channels.** While wiring was being fixed, individual inputs could be disabled
  in firmware so the rest stayed usable.
- **Leaky Mode button.** The Mode pad leaked about 550 ohms to ground even when released.
  It was cut free of the old board and wired straight between GPIO20 and GND.
- **Flickering buttons.** One input had a floating pull-down after rework; resoldering it
  stopped the flicker.
- **Weak sticks.** The sticks were getting 1.8 V instead of 3.3 V. Feeding them from the
  3V3 pin restored the full range.
- **Calibration.** A browser wizard maps every button and measures the sticks, and the
  result was written into the firmware as the default.
- **Trusted and pairing modes.** The pad now reconnects only to devices it has paired,
  and L1 + R1 + Mode opens pairing. In pairing mode it advertises from a private address
  with a fresh key, so already-paired phones do not keep reconnecting.
- **Wired mode.** The ESP32-C3's USB port cannot be a USB gamepad, so in wired mode the
  pad streams its state to a small Linux helper that creates a virtual Xbox 360 pad.

## Result

All 16 buttons, the Mode button and both analog sticks work, over Bluetooth and over
USB. The controller pairs with phones and laptops as an Xbox controller and reconnects
to them automatically.
