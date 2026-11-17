# Quantum Pad backup, 2026-11-13: 5 inputs disabled
Disabled in firmware: wizard bars 2, 6, 13, 15 (raw bits 1, 5, 12, 14) and Mode (GPIO20).
Xbox Series X|S mode (045E:0B13), name in scan response.

Restore firmware (battery OFF, USB in):
  arduino-cli upload --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc -p /dev/ttyACM0 --input-dir firmware/archive/2026-11-13_5-disabled/build firmware/quantum_pad   # run from the repository root

Restore calibration (if the pad lost it): with tools/live.py running,
  curl --data-binary @calibration_commands.txt http://localhost:8765/
