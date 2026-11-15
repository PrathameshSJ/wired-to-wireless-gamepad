# Capture the SuperMini's USB serial output for N seconds.
# Resets it into normal boot first (DTR low, pulse RTS) unless --noreset is given.
import os, fcntl, termios, struct, time, sys
fd = os.open(sys.argv[1], os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
a = termios.tcgetattr(fd); a[3] = 0; a[0] = 0; a[1] = 0; termios.tcsetattr(fd, termios.TCSANOW, a)
TIOCMBIS, TIOCMBIC, DTR, RTS = 0x5416, 0x5417, 0x002, 0x004
def setb(bits, on): fcntl.ioctl(fd, TIOCMBIS if on else TIOCMBIC, struct.pack('I', bits))
if '--noreset' not in sys.argv:
    setb(DTR, False); setb(RTS, True); time.sleep(0.1); setb(RTS, False)
buf = b''; t = time.time()
while time.time() - t < float(sys.argv[2]):
    try: buf += os.read(fd, 4096)
    except (BlockingIOError, OSError): time.sleep(0.05)
print(buf.decode(errors='replace')[-8000:])
