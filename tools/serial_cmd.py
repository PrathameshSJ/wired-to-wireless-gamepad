#!/usr/bin/env python3
"""Send serial commands to the pad over USB and print its replies.

    python3 tools/serial_cmd.py GET            # print the calibration
    python3 tools/serial_cmd.py BONDS          # list saved devices
    python3 tools/serial_cmd.py TRUST          # switch on Bluetooth (trusted mode)
    python3 tools/serial_cmd.py WIPE           # forget every saved device
    python3 tools/serial_cmd.py --wait 10      # just watch the pad's log for 10 s

Commands are listed at the top of firmware/quantum_pad/quantum_pad.ino.
The periodic status lines (S ... / G ...) are hidden unless --all is given.
Opening the port does not reset the pad.
"""
import argparse, glob, os, select, sys, termios, time


def find_port():
    for tty in sorted(glob.glob('/sys/class/tty/ttyACM*')):
        try:
            if open(os.path.realpath(tty + '/device/..') + '/idVendor').read().strip() == '303a':
                return '/dev/' + os.path.basename(tty)
        except OSError:
            pass
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('commands', nargs='*', help='commands to send, in order')
    ap.add_argument('--port', help='serial port (default: the ESP32-C3 found on USB)')
    ap.add_argument('--wait', type=float, default=1.5, help='seconds to collect replies (default 1.5)')
    ap.add_argument('--all', action='store_true', help='also show the periodic S/G status lines')
    a = ap.parse_args()

    port = a.port or find_port()
    if not port:
        sys.exit('pad not found on USB')
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY)
    t = termios.tcgetattr(fd)
    t[0] = t[1] = t[3] = 0                       # raw
    t[2] &= ~termios.HUPCL                       # don't reset the pad on close
    termios.tcsetattr(fd, termios.TCSANOW, t)

    os.write(fd, b'\n')                          # flush any half-typed line
    for c in a.commands:
        os.write(fd, c.encode() + b'\n')
        time.sleep(0.1)

    buf, end = b'', time.time() + a.wait
    while time.time() < end:
        if select.select([fd], [], [], 0.2)[0]:
            buf += os.read(fd, 4096)
    for line in buf.decode(errors='replace').splitlines():
        if line and (a.all or not line.startswith(('S ', 'G '))):
            print(line)


if __name__ == '__main__':
    main()
