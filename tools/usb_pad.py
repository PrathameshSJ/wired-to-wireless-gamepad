#!/usr/bin/env python3
"""USB helper for the pad's WIRED mode (Linux).

The ESP32-C3's USB port can only be a serial port, not a gamepad. In WIRED mode
the pad streams its state over that serial port as text lines

    G <buttons hex> <lx> <ly> <rx> <ry> <lt> <rt>

and this script turns them into a virtual Xbox 360 controller through uinput,
so games, Steam and SDL see a normal wired Xbox pad.

    python3 tools/usb_pad.py            # Ctrl+C to stop
    python3 tools/usb_pad.py --invert-y # if up/down come out reversed
    python3 tools/usb_pad.py --bt-off   # also switch the pad to WIRED (Bluetooth off) on connect

While it runs, type a letter and Enter to switch the pad's link mode:
    o   Bluetooth OFF  (WIRED, play through this helper)
    t   Bluetooth on, TRUSTED (saved devices only)
    p   Bluetooth on, PAIRING (pair a new device)
    s   show status (mode, saved devices)

Needs python-evdev and write access to /dev/uinput (the `input` group).
The virtual pad exists only while the pad is plugged in and in WIRED mode; it
disappears when you switch the pad to Bluetooth, so you never get two pads.
Don't run it together with tools/live.py: both read the same serial port.
"""
import argparse, glob, os, select, sys, termios, time

from evdev import AbsInfo, UInput, ecodes as e

# Bit order sent by the firmware (sendWired in quantum_pad.ino).
BUTTONS = [e.BTN_A, e.BTN_B, e.BTN_X, e.BTN_Y, e.BTN_TL, e.BTN_TR,
           e.BTN_THUMBL, e.BTN_THUMBR, e.BTN_SELECT, e.BTN_START, e.BTN_MODE]
UP, DOWN, LEFT, RIGHT = 11, 12, 13, 14
STALE_S = 1.0       # no G line for this long while status lines still come = pad left WIRED
SILENT_S = 3.0      # no data at all for this long = pad hung or the link died

STICK = AbsInfo(0, -32768, 32767, 16, 128, 0)
TRIGGER = AbsInfo(0, 0, 255, 0, 0, 0)
HAT = AbsInfo(0, -1, 1, 0, 0, 0)
CAPS = {
    e.EV_KEY: BUTTONS,
    e.EV_ABS: [(e.ABS_X, STICK), (e.ABS_Y, STICK), (e.ABS_RX, STICK), (e.ABS_RY, STICK),
               (e.ABS_Z, TRIGGER), (e.ABS_RZ, TRIGGER),
               (e.ABS_HAT0X, HAT), (e.ABS_HAT0Y, HAT)],
}


def find_port():
    """The pad's serial port: the ESP32-C3's built-in USB (Espressif 303a:1001)."""
    for tty in sorted(glob.glob('/sys/class/tty/ttyACM*')):
        try:
            usb = os.path.realpath(tty + '/device/..')
            if open(usb + '/idVendor').read().strip() == '303a':
                return '/dev/' + os.path.basename(tty)
        except OSError:
            pass
    return None


def open_port(path):
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    a = termios.tcgetattr(fd)
    a[0] = a[1] = a[3] = 0                      # raw
    a[2] &= ~termios.HUPCL                      # don't reset the pad on close
    termios.tcsetattr(fd, termios.TCSANOW, a)
    return fd


def log(msg):
    print(time.strftime('%H:%M:%S'), msg, flush=True)


class Pad:
    def __init__(self, invert_y):
        self.ui = None
        self.invert_y = invert_y

    def open(self):
        if not self.ui:
            self.ui = UInput(CAPS, name='Microsoft X-Box 360 pad', vendor=0x045E,
                             product=0x028E, version=0x0110, bustype=e.BUS_USB)
            log('virtual Xbox pad created: ' + self.ui.device.path)

    def close(self):
        if self.ui:
            self.ui.close()
            self.ui = None
            log('virtual Xbox pad removed')

    def update(self, bits, lx, ly, rx, ry, lt, rt):
        self.open()
        w = self.ui.write
        for i, code in enumerate(BUTTONS):
            w(e.EV_KEY, code, (bits >> i) & 1)
        if self.invert_y:
            ly, ry = -ly, -ry
        w(e.EV_ABS, e.ABS_X, lx); w(e.EV_ABS, e.ABS_Y, ly)
        w(e.EV_ABS, e.ABS_RX, rx); w(e.EV_ABS, e.ABS_RY, ry)
        w(e.EV_ABS, e.ABS_Z, lt); w(e.EV_ABS, e.ABS_RZ, rt)
        b = lambda n: (bits >> n) & 1
        w(e.EV_ABS, e.ABS_HAT0X, b(RIGHT) - b(LEFT))
        w(e.EV_ABS, e.ABS_HAT0Y, b(DOWN) - b(UP))
        self.ui.syn()


KEYS = {'o': b'WIRED', 'w': b'WIRED', 't': b'TRUST', 'p': b'PAIR', 's': b'BONDS'}


def handle_key(fd, line):
    cmd = KEYS.get(line.strip().lower()[:1])
    if cmd:
        os.write(fd, cmd + b'\n')
    elif line.strip():
        log('keys: o = Bluetooth off (wired), t = trusted, p = pairing, s = status')


def run(pad, bt_off):
    waiting = False
    while True:
        port = find_port()
        if not port:
            if not waiting:
                log('waiting for the pad on USB...')
                waiting = True
            time.sleep(1)
            continue
        waiting = False
        try:
            fd = open_port(port)
        except OSError as err:
            log(f'cannot open {port}: {err}')
            time.sleep(2)
            continue
        log(f'listening on {port}  (o = Bluetooth off/wired, t = trusted, p = pairing, s = status)')
        if bt_off:
            os.write(fd, b'WIRED\n')
        buf, last_g, last_any = b'', 0.0, time.monotonic()
        last_hi = 0.0
        watch = [fd] + ([sys.stdin] if sys.stdin.isatty() else [])
        try:
            while True:
                ready = select.select(watch, [], [], 0.2)[0]
                if sys.stdin in ready:
                    handle_key(fd, sys.stdin.readline())
                if fd in ready:
                    chunk = os.read(fd, 4096)
                    if not chunk:
                        raise OSError('port closed')
                    buf += chunk
                    last_any = time.monotonic()
                    *lines, buf = buf.split(b'\n')
                    for ln in lines:
                        if ln.startswith(b'G '):
                            f = ln.split()
                            if len(f) == 8:
                                try:
                                    pad.update(int(f[1], 16), *map(int, f[2:]))
                                    last_g = time.monotonic()
                                except ValueError:
                                    pass
                        elif ln.startswith((b'LINK', b'BONDS', b'ERR', b'OK')):
                            log('pad: ' + ln.decode(errors='replace').strip())
                now = time.monotonic()
                if now - last_hi > 0.5:          # keep-alive: tells the pad a computer is listening
                    os.write(fd, b'HI\n')
                    last_hi = now
                if pad.ui and now - last_any > SILENT_S:
                    log(f'no data from the pad for {SILENT_S:.0f} s')
                    pad.close()
                elif pad.ui and now - last_g > STALE_S and now - last_any < STALE_S:
                    log('pad stopped wired output (it switched to Bluetooth)')
                    pad.close()
        except OSError:
            log('pad unplugged')
        finally:
            pad.close()
            try:
                os.close(fd)
            except OSError:
                pass
        time.sleep(1)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--invert-y', action='store_true', help='flip both sticks vertically')
    ap.add_argument('--bt-off', action='store_true', help='switch the pad to WIRED (Bluetooth off) whenever it connects')
    args = ap.parse_args()
    if not os.access('/dev/uinput', os.W_OK):
        sys.exit('no write access to /dev/uinput: add yourself to the "input" group, or run with sudo')
    pad = Pad(args.invert_y)
    try:
        run(pad, args.bt_off)
    except KeyboardInterrupt:
        pad.close()


if __name__ == '__main__':
    main()
