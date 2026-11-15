"""Live button tester: reads the pad over USB serial, or over Bluetooth (the
"Quantum Pad" input device) when USB is not connected, and serves a page at
http://localhost:8765 showing the 16 shift-register inputs as bars.
Usage: python3 live.py [/dev/ttyACM0]"""
import os, sys, time, threading, json, re, termios, glob, select, struct, fcntl
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler

PORT_ARG = sys.argv[1] if len(sys.argv) > 1 else None   # default: first /dev/ttyACM*
HERE = os.path.dirname(os.path.abspath(__file__))
state = {'bits': 0, 'mode': 0, 'sticks': [2048] * 4, 'connected': False, 'source': ''}
usb_live = False   # USB serial wins when both are present
ser_fd = None      # open USB serial port, used to send calibration commands
CFG = re.compile(rb'^CFG (\{.*\})')
PROBE = re.compile(rb'^PROBE ([01]) ([01])')
REPLY = re.compile(rb'^(OK|ERR) ?(.*)')
cond = threading.Condition()
LINE = re.compile(rb'^S ([0-9A-F]{4}) ([01])(?: (\d+) (\d+) (\d+) (\d+))?')

TIOCMBIS, TIOCMBIC, TIOCM_DTR, TIOCM_RTS = 0x5416, 0x5417, 0x002, 0x004
def recover(fd):
    """Normal-boot reset of the ESP32-C3: DTR low (no download mode), pulse RTS."""
    try:
        fcntl.ioctl(fd, TIOCMBIC, struct.pack('I', TIOCM_DTR))
        fcntl.ioctl(fd, TIOCMBIS, struct.pack('I', TIOCM_RTS)); time.sleep(0.1)
        fcntl.ioctl(fd, TIOCMBIC, struct.pack('I', TIOCM_RTS))
        print('pad went quiet: sent a normal reset', flush=True)
    except OSError:
        pass

def reader():
    global usb_live, ser_fd
    while True:
        try:
            ports = [PORT_ARG] if PORT_ARG else sorted(glob.glob('/dev/ttyACM*'))
            if not ports: raise OSError('no port')
            fd = os.open(ports[0], os.O_RDWR | os.O_NOCTTY)
            a = termios.tcgetattr(fd)               # raw mode: no line editing or echo
            a[0] = a[1] = a[3] = 0
            a[2] &= ~termios.HUPCL                  # don't toggle DTR/RTS on close: that can drop the C3 into download mode
            a[6][termios.VMIN], a[6][termios.VTIME] = 1, 0
            termios.tcsetattr(fd, termios.TCSANOW, a)
            ser_fd = fd
            os.write(fd, b'GET\n')
        except OSError:
            time.sleep(1); continue
        buf = b''
        quiet = 0
        try:
            while True:
                # The pad sends a state line every 25 ms. If it goes quiet while the
                # port is still there, keep the port open (reopening toggles DTR/RTS)
                # and after 3 s do a clean normal-boot reset to recover it.
                if not select.select([fd], [], [], 1.0)[0]:
                    if not os.path.exists(ports[0]): raise OSError('gone')
                    usb_live = False
                    with cond:
                        if state.get('source') == 'USB': state['connected'] = False
                        cond.notify_all()
                    quiet += 1
                    if quiet >= 3:
                        recover(fd); quiet = 0
                    continue
                quiet = 0
                chunk = os.read(fd, 4096)
                if not chunk: raise OSError('closed')
                buf += chunk
                *lines, buf = buf.split(b'\n')
                for ln in lines:
                    ln = ln.strip()
                    c = CFG.match(ln) or PROBE.match(ln) or REPLY.match(ln)
                    if c:
                        with cond:
                            if c.re is CFG: state['cfg'] = json.loads(c[1])
                            elif c.re is PROBE: state['probe'] = [int(c[1]), int(c[2]), state.get('probe', [0, 0, 0])[2] + 1]
                            else: state['reply'] = [c[1].decode() + ' ' + c[2].decode(), state.get('reply', ['', 0])[1] + 1]
                            cond.notify_all()
                        continue
                    m = LINE.match(ln)
                    if m:
                        usb_live = True
                        with cond:
                            state.update(bits=int(m[1], 16), mode=int(m[2]), connected=True, source='USB')
                            if m[3]: state['sticks'] = [int(m[k]) for k in range(3, 7)]
                            cond.notify_all()
        except OSError:
            ser_fd = None
            os.close(fd)
            usb_live = False
            with cond:
                state['connected'] = False; cond.notify_all()
            time.sleep(1)


# ---- Bluetooth: read the kernel input device the pad creates when connected ----
# Firmware HID button n arrives as key code 303+n; map back to the raw shift-register bit.
# (Pad presents as an Xbox Series controller: A B _ X Y _ LB RB _ _ View Menu Guide LS RS.)
HID_TO_BIT = {15: 0, 14: 1, 12: 2, 11: 3, 8: 10, 7: 11, 4: 12, 1: 13, 2: 14, 5: 15}
TRIGGER_BIT = {10: 9, 9: 8}   # ABS_BRAKE = left trigger -> L2, ABS_GAS = right trigger -> R2
HID_MODE = 13
HAT_BITS = {'R': 4, 'L': 5, 'D': 6, 'U': 7}
EV = struct.Struct('qqHHi')
AXIS_SLOT = {0: 0, 1: 1, 2: 2, 5: 3}   # ABS_X, ABS_Y -> left stick; ABS_Z, ABS_RZ -> right stick

def find_bt():
    for d in glob.glob('/sys/class/input/event*'):
        try:
            if any(k in open(d + '/device/name').read() for k in ('Quantum', 'PSJ', 'Xbox Wireless Controller')): return '/dev/input/' + os.path.basename(d)
        except OSError: pass
    return None

def bt_reader():
    while True:
        path = find_bt()
        if not path or usb_live:
            time.sleep(1); continue
        try: fd = os.open(path, os.O_RDONLY)
        except OSError: time.sleep(1); continue
        bits, mode, sticks, hat = 0, 0, [2048] * 4, [0, 0]
        with cond:
            state.update(bits=0, mode=0, sticks=sticks[:], connected=True, source='Bluetooth'); cond.notify_all()
        try:
            while not usb_live:
                if not select.select([fd], [], [], 1.0)[0]:
                    if not os.path.exists(path): raise OSError('gone')
                    continue
                data = os.read(fd, EV.size * 64)
                if not data: raise OSError('closed')
                for i in range(0, len(data) - EV.size + 1, EV.size):
                    _, _, typ, code, val = EV.unpack_from(data, i)
                    if typ == 1 and code == 172:   # Guide may arrive as KEY_HOMEPAGE
                        mode = 1 if val else 0
                    elif typ == 3 and code in TRIGGER_BIT:
                        b = 1 << TRIGGER_BIT[code]
                        bits = bits | b if val > 0 else bits & ~b
                    elif typ == 1 and 304 <= code <= 319:
                        n = code - 303
                        if n == HID_MODE: mode = 1 if val else 0
                        elif n in HID_TO_BIT:
                            b = 1 << HID_TO_BIT[n]
                            bits = bits | b if val else bits & ~b
                    elif typ == 3 and code in (16, 17):
                        hat[code - 16] = val
                    elif typ == 3 and code in AXIS_SLOT:
                        sticks[AXIS_SLOT[code]] = max(0, min(4095, val // 16))   # 0..65535 -> ADC-like 0..4095
                    elif typ == 0:   # end of one report: publish
                        hb = 0
                        if hat[0] > 0: hb |= 1 << HAT_BITS['R']
                        if hat[0] < 0: hb |= 1 << HAT_BITS['L']
                        if hat[1] > 0: hb |= 1 << HAT_BITS['D']
                        if hat[1] < 0: hb |= 1 << HAT_BITS['U']
                        with cond:
                            state.update(bits=(bits & ~0xF0) | hb, mode=mode, sticks=sticks[:],
                                         connected=True, source='Bluetooth')
                            cond.notify_all()
        except OSError:
            pass
        os.close(fd)
        if not usb_live:
            with cond:
                state['connected'] = False; cond.notify_all()
        time.sleep(1)

class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        # Calibration commands from the page, one per line, forwarded to the pad over USB.
        body = self.rfile.read(int(self.headers.get('Content-Length', 0)))
        fd = ser_fd
        ok = fd is not None
        if ok:
            try:
                for ln in body.decode().splitlines():
                    ln = ln.strip()
                    if re.fullmatch(r'[A-Z]+( -?\d+)*', ln):
                        os.write(fd, ln.encode() + b'\n'); time.sleep(0.02)
            except OSError: ok = False
        self.send_response(200 if ok else 409)
        self.send_header('Content-Type', 'text/plain'); self.end_headers()
        self.wfile.write(b'sent' if ok else b'USB not connected')
    def do_GET(self):
        if self.path == '/events':
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.end_headers()
            last = None
            try:
                while True:
                    with cond:
                        cond.wait(timeout=1)
                        snap = json.dumps(state)
                    if snap != last:
                        self.wfile.write(f'data: {snap}\n\n'.encode()); self.wfile.flush(); last = snap
            except (BrokenPipeError, ConnectionResetError):
                return
        page = os.path.join(HERE, 'live.html')
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.end_headers()
        with open(page, 'rb') as f: self.wfile.write(f.read())

threading.Thread(target=reader, daemon=True).start()
threading.Thread(target=bt_reader, daemon=True).start()
print('Live tester on http://localhost:8765', flush=True)
ThreadingHTTPServer(('127.0.0.1', 8765), Handler).serve_forever()
