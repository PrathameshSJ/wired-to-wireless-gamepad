// Quantum Pad: ESP32-C3 SuperMini BLE gamepad.
// Named "PSJ CUSTOM", but uses the Xbox Series X|S IDs (045E:0B13, the
// genuine Xbox BLE HID layout), so Android, iOS, Windows, Linux and consoles'
// game layers map it as an Xbox pad with no setup. L2/R2 are full-press triggers.
// Bonds to up to 3 hosts; whichever bonded host is in range reconnects on boot.
// Buttons are active-high (common = 3.3V, 10k pull-downs) read through two
// daisy-chained 74HC165s. Sticks are pots on 3.3V into ADC1.
// Avoid GPIO2/8/9: strapping pins (a stick on GPIO2 forced download-mode boot).
//
// Calibration (button map, stick ranges, deadzone, axis inversion, Mode wiring)
// lives in flash and is set from the tester page over USB serial:
//   GET                      print config as  CFG {json}
//   MAP b0 b1 ... b15        what each raw input is (Btn number, 16 = unused)
//   CAL axis min centre max  stick range in raw ADC counts (centre -1 = auto)
//   DZ n                     deadzone in raw ADC counts
//   INV mask                 bit per axis (LX LY RX RY) to invert
//   MODEPOL p                0 = Mode button goes to GND, 1 = goes to 3.3V
//   SWAP mask                bit0: left stick X/Y wires swapped, bit1: right stick
//   PROBE                    read GPIO20 with pull-up and pull-down: PROBE up down
//   SAVE / DEFAULTS
//   PAIR / TRUST / WIRED     switch link mode (same as the L1 + R1 + Mode combo)
//   BONDS                    list the saved (trusted) devices
//   FORGET                   delete every saved device
//
// Link modes (onboard blue LED on GPIO8 shows which). Hold L1 + R1 + Mode for 3 s
// to step to the next one:
//   on USB:      WIRED -> TRUSTED -> PAIRING -> WIRED ...
//   on battery:  TRUSTED <-> PAIRING
// Power-on picks the start mode: plugged into a computer = WIRED, otherwise TRUSTED.
// Plugging USB in while the pad is already running changes nothing (top up the
// battery mid-game and stay on Bluetooth). Unplugging while WIRED drops to TRUSTED.
//   WIRED                 Bluetooth off. The pad streams its state over the USB
//                         serial port as  G <buttons hex> <lx> <ly> <rx> <ry> <lt> <rt>
//                         and tools/usb_pad.py on the computer turns that into a
//                         virtual Xbox controller (the C3's USB can't be a real one).
//                         LED: steady on.
//   TRUSTED (Bluetooth)   hidden from scan lists; only already-paired devices may
//                         connect, anything else is dropped without being saved.
//                         LED: short blip every 2 s while waiting, off when connected.
//   PAIRING               hold L1 + R1 + Mode for 3 s. Drops the current device,
//                         advertises from a private address made with a fresh key
//                         (so saved devices don't recognise it and don't keep
//                         retrying), shows
//                         up in scan lists and in Windows' Swift Pair popup, and
//                         saves whatever pairs. LED: fast blink.
//                         Hold the combo again to lock back to TRUSTED with the new
//                         device trusted. Falls back by itself after 3 min with
//                         nothing connected. Up to 3 devices; a 4th replaces the oldest.
#include <Arduino.h>
#include <BleGamepad.h>
#include <NimBLEDevice.h>
#include <algorithm>
#include <map>
#include <vector>
#include <Preferences.h>

const int PIN_LX = 0, PIN_LY = 1, PIN_RY = 3, PIN_RX = 4;
const int PIN_LOAD = 5, PIN_CLK = 6, PIN_DATA = 7;
const int PIN_MODE = 20;
// Mode's carbon pad leaks ~550 ohm to GND even when released (<100 ohm pressed),
// which beats any internal pull-up. A 220 ohm resistor from GPIO10 to GPIO20
// makes a divider (released ~2.4 V = high, pressed <=1.0 V = low); GPIO10 is only
// driven for ~20 us per read so the leak costs ~10 uA on average. Without the
// resistor this behaves exactly like the plain pull-up read.
const int PIN_MODE_DRIVE = 10;

enum Btn : uint8_t { UP, DOWN, LEFT, RIGHT, SOUTH, EAST, NORTH, WEST,
                     L1, R1, L2, R2, SELECT, START, L3, R3, NONE };
const char *BTN_NAME[] = { "UP", "DOWN", "LEFT", "RIGHT", "X(south)", "O(east)", "TRI(north)", "SQ(west)",
                           "L1", "R1", "L2", "R2", "SELECT", "START", "L3", "R3", "-" };
// Default wiring: raw shift order is chip1 H..A, then chip2 H..A.
// Built-in defaults = the calibration saved from the wizard on 2026-11-14, so
// DEFAULTS (or a wiped flash) lands on a working pad. Raw input 12 (wizard bar 13)
// is unassigned, so D-pad UP has no input yet.
const uint8_t DEFAULT_MAP[16] = { NORTH, SOUTH, EAST, WEST, SELECT, START, L3, R3,
                                  L1, L2, R1, R2, NONE, LEFT, RIGHT, DOWN };
const int16_t DEFAULT_MIN[4] = { 170, 168, 167, 167 };
const int16_t DEFAULT_CTR[4] = { 2328, 2199, 2281, 2310 };
const int16_t DEFAULT_MAX[4] = { 3931, 3931, 3931, 3931 };
const uint8_t DEFAULT_SWAP = 2;   // right stick X/Y wires swapped

// Library XInput button numbers: 1 A, 2 B, 3 X, 4 Y, 5 LB, 6 RB, 7 LS, 8 RS,
// 9 View, 10 Menu, 11 Xbox. Face buttons by position: south = A, east = B, west = X, north = Y.
uint8_t hidButton(Btn b) {
  switch (b) {
    case SOUTH: return 1;  case EAST: return 2;  case WEST: return 3;  case NORTH: return 4;
    case L1: return 5;     case R1: return 6;
    case SELECT: return 9; case START: return 10; case L3: return 7;  case R3: return 8;
    default: return 0;     // L2/R2 are analog triggers, sent separately
  }
}
const uint8_t HID_MODE = 11;

struct Config {
  uint8_t ver;
  uint8_t map[16];
  int16_t amin[4], actr[4], amax[4];
  int16_t dz;
  uint8_t inv;
  uint8_t modePol;
};
const uint8_t CFG_VER = 2;
Config conf;
uint8_t swapMask = 0;   // stored separately (key "swap") so older saved configs stay valid
Preferences prefs;

// Inputs that read pressed at power-on are stuck (broken pull-down, or Mode's
// line held low): ignore each one until it has been seen released once.
uint16_t stuckRaw = 0;
bool modeStuck = false;
const uint32_t COMBO_MS = 1000;   // hold L3 + R3 this long = Mode button

const char *PAD_NAME = "PSJ CUSTOM";
BleGamepad bleGamepad(PAD_NAME, "Microsoft", 100);   // XInput mode overrides this name in begin(); renamed again in setup()
BleGamepadConfiguration bcfg;
int centre[4];
const int stickPin[4] = { PIN_LX, PIN_LY, PIN_RX, PIN_RY };

void setDefaults() {
  conf.ver = CFG_VER;
  memcpy(conf.map, DEFAULT_MAP, 16);
  for (int a = 0; a < 4; a++) { conf.amin[a] = DEFAULT_MIN[a]; conf.actr[a] = DEFAULT_CTR[a]; conf.amax[a] = DEFAULT_MAX[a]; }
  conf.dz = 120; conf.inv = 0; conf.modePol = 0;
  swapMask = DEFAULT_SWAP;
}

void applyModePin() {
  pinMode(PIN_MODE, conf.modePol ? INPUT_PULLDOWN : INPUT_PULLUP);
  pinMode(PIN_MODE_DRIVE, INPUT);   // hi-Z between reads
}

// Raw Mode level, pulsing the strong pull-up on GPIO10 for GND-wired Mode.
bool readModeRaw() {
  if (conf.modePol) return digitalRead(PIN_MODE);
  pinMode(PIN_MODE_DRIVE, OUTPUT);
  digitalWrite(PIN_MODE_DRIVE, HIGH);
  delayMicroseconds(20);
  bool pressed = !digitalRead(PIN_MODE);
  pinMode(PIN_MODE_DRIVE, INPUT);
  return pressed;
}

// Debounced: the carbon pad sits close to the threshold while being pressed,
// so only change state after 3 identical reads in a row (~25 ms).
// Mode is switched off until its pad is fixed or the GPIO10 resistor is fitted
// (set to false then). L3 + R3 held for a second still acts as Mode.
const bool MODE_DISABLED = false;

bool readMode() {
  if (MODE_DISABLED) return false;
  static bool state = false;
  static uint8_t same = 0;
  bool r = readModeRaw();
  if (r == state) same = 0;
  else if (++same >= 3) { state = r; same = 0; }
  return state;
}

int readAvg(int pin) {
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(pin);
  return sum / 8;
}

// Stick values in logical order (LX LY RX RY), after undoing swapped wires.
void readSticks(int v[4]) {
  for (int a = 0; a < 4; a++) v[a] = readAvg(stickPin[a]);
  if (swapMask & 1) { int t = v[0]; v[0] = v[1]; v[1] = t; }
  if (swapMask & 2) { int t = v[2]; v[2] = v[3]; v[3] = t; }
}

// Re-centre at boot to cancel drift, unless the stick is clearly being held
// (then trust the saved centre).
void measureCentres() {
  int v[4];
  readSticks(v);
  for (int a = 0; a < 4; a++) {
    int c = v[a];
    if (conf.actr[a] >= 0 && abs(c - conf.actr[a]) > 300) c = conf.actr[a];
    centre[a] = c;
  }
}

int16_t toAxis(int raw, int a) {
  int c = centre[a], d = raw - c;
  long v;
  if (abs(d) < conf.dz) v = 16384;
  else if (d < 0) v = 16384L + (long)(d + conf.dz) * 16384 / max(c - conf.dz - conf.amin[a], 1);
  else            v = 16384L + (long)(d - conf.dz) * 16383 / max(conf.amax[a] - c - conf.dz, 1);
  v = constrain(v, 0, 32767);
  if (conf.inv & (1 << a)) v = 32767 - v;
  return v;
}

// XInput sticks are signed, centred on 0 (up / left = negative).
int16_t xAxis(int raw, int a) { return constrain(((long)toAxis(raw, a) - 16384) * 2, -32767L, 32767L); }

// Inputs permanently switched off (bit n = wizard bar n+1). None at the moment;
// e.g. (1u << 1) | (1u << 5) switches off bars 2 and 6.
// they always read as released, for the wizard and over Bluetooth alike.
const uint16_t DISABLED_RAW = 0;

uint16_t readShift() {
  digitalWrite(PIN_LOAD, LOW);
  delayMicroseconds(5);
  digitalWrite(PIN_LOAD, HIGH);
  delayMicroseconds(5);
  uint16_t bits = 0;
  for (int i = 0; i < 16; i++) {
    if (digitalRead(PIN_DATA)) bits |= 1u << i;
    digitalWrite(PIN_CLK, HIGH);
    delayMicroseconds(5);
    digitalWrite(PIN_CLK, LOW);
    delayMicroseconds(5);
  }
  return bits & ~DISABLED_RAW;
}

uint8_t hatFrom(bool u, bool d, bool l, bool r) {
  if (u && r) return HAT_UP_RIGHT;   if (u && l) return HAT_UP_LEFT;
  if (d && r) return HAT_DOWN_RIGHT; if (d && l) return HAT_DOWN_LEFT;
  if (u) return HAT_UP; if (d) return HAT_DOWN; if (l) return HAT_LEFT; if (r) return HAT_RIGHT;
  return HAT_CENTERED;
}

void printCfg() {
  Serial.print("CFG {\"map\":[");
  for (int i = 0; i < 16; i++) Serial.printf("%s%d", i ? "," : "", conf.map[i]);
  Serial.print("],\"min\":[");
  for (int a = 0; a < 4; a++) Serial.printf("%s%d", a ? "," : "", conf.amin[a]);
  Serial.print("],\"ctr\":[");
  for (int a = 0; a < 4; a++) Serial.printf("%s%d", a ? "," : "", conf.actr[a]);
  Serial.print("],\"max\":[");
  for (int a = 0; a < 4; a++) Serial.printf("%s%d", a ? "," : "", conf.amax[a]);
  Serial.print("],\"centre\":[");
  for (int a = 0; a < 4; a++) Serial.printf("%s%d", a ? "," : "", centre[a]);
  Serial.printf("],\"dz\":%d,\"inv\":%d,\"modePol\":%d,\"swap\":%d}\n", conf.dz, conf.inv, conf.modePol, swapMask);
}

extern bool bleStarted;   // defined with the link modes below
void handleCmd(const char *l) {
  int a, b, c, d;
  if (!strcmp(l, "GET")) { printCfg(); return; }
  if (!strncmp(l, "MAP ", 4)) {
    int v[16], n = 0;
    const char *p = l + 4;
    while (n < 16) { char *e; long x = strtol(p, &e, 10); if (e == p) break; v[n++] = x; p = e; }
    if (n != 16) { Serial.println("ERR MAP needs 16 numbers"); return; }
    for (int i = 0; i < 16; i++) conf.map[i] = (v[i] >= 0 && v[i] <= NONE) ? v[i] : NONE;
    Serial.println("OK MAP"); return;
  }
  if (sscanf(l, "CAL %d %d %d %d", &a, &b, &c, &d) == 4 && a >= 0 && a < 4) {
    conf.amin[a] = b; conf.actr[a] = c; conf.amax[a] = d;
    if (c >= 0) centre[a] = c;
    Serial.println("OK CAL"); return;
  }
  if (sscanf(l, "DZ %d", &a) == 1) { conf.dz = constrain(a, 0, 800); Serial.println("OK DZ"); return; }
  if (sscanf(l, "INV %d", &a) == 1) { conf.inv = a & 15; Serial.println("OK INV"); return; }
  if (sscanf(l, "MODEPOL %d", &a) == 1) { conf.modePol = a ? 1 : 0; applyModePin(); Serial.println("OK MODEPOL"); return; }
  if (sscanf(l, "SWAP %d", &a) == 1) {
    uint8_t ch = (a & 3) ^ swapMask;   // keep live centres in logical order
    if (ch & 1) { int t = centre[0]; centre[0] = centre[1]; centre[1] = t; }
    if (ch & 2) { int t = centre[2]; centre[2] = centre[3]; centre[3] = t; }
    swapMask = a & 3; Serial.println("OK SWAP"); return;
  }
  if (!strcmp(l, "PROBE")) {
    pinMode(PIN_MODE, INPUT_PULLUP);   delay(5); int up = digitalRead(PIN_MODE);
    pinMode(PIN_MODE, INPUT_PULLDOWN); delay(5); int dn = digitalRead(PIN_MODE);
    applyModePin();
    Serial.printf("PROBE %d %d\n", up, dn); return;
  }
  if (!strcmp(l, "HI")) return;   // keep-alive from tools/usb_pad.py
  if (!strcmp(l, "PAIR"))  { setPairing(true);  return; }
  if (!strcmp(l, "TRUST")) { setPairing(false); return; }
  if (!strcmp(l, "WIRED")) { setWired(); return; }
  if ((!strcmp(l, "BONDS") || !strcmp(l, "FORGET")) && !bleStarted) { Serial.println("ERR bluetooth is off (WIRED mode)"); return; }
  if (!strcmp(l, "BONDS")) {
    Serial.printf("BONDS %d:", NimBLEDevice::getNumBonds());
    for (int i = 0; i < NimBLEDevice::getNumBonds(); i++) Serial.printf(" %s", NimBLEDevice::getBondedAddress(i).toString().c_str());
    Serial.println();
    return;
  }
  if (!strcmp(l, "FORGET")) { NimBLEDevice::deleteAllBonds(); snapshotTrusted(); Serial.println("OK FORGET"); return; }
  if (!strcmp(l, "SAVE")) { prefs.putBytes("cfg", &conf, sizeof conf); prefs.putUChar("swap", swapMask); Serial.println("OK SAVE"); printCfg(); return; }
  if (!strcmp(l, "DEFAULTS")) { setDefaults(); applyModePin(); measureCentres(); Serial.println("OK DEFAULTS"); printCfg(); return; }
  Serial.printf("ERR unknown: %s\n", l);
}

uint32_t lastRx = 0;   // when the computer last sent us anything
void pollSerial() {
  static char buf[160];
  static int n = 0;
  while (Serial.available()) {
    lastRx = millis();
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') { buf[n] = 0; if (n) handleCmd(buf); n = 0; }
    else if (n < (int)sizeof buf - 1) buf[n++] = ch;
  }
}

// ---- Link modes: TRUSTED / PAIRING ----
const int PIN_LED = 8;                     // SuperMini blue LED, active-low (only driven after boot)
const uint32_t PAIR_HOLD_MS = 3000;        // L1 + R1 + Mode held this long toggles the mode
const uint32_t PAIR_TIMEOUT_MS = 180000;   // pairing mode gives up after 3 min with nothing connected
const uint32_t ENCRYPT_WAIT_MS = 8000;     // a paired device encrypts within ~1 s of connecting
bool pairingMode = false;
bool wiredMode = false;                    // WIRED: Bluetooth off, state streamed over USB serial
bool bleStarted = false;                   // Bluetooth is only started once it's first needed
uint32_t pairingSince = 0;
std::vector<NimBLEAddress> trusted;        // devices saved when TRUSTED mode was entered
std::vector<NimBLEAddress> excluded;       // devices dropped on entering PAIRING mode
std::map<uint16_t, uint32_t> connSince;    // connection handle -> when it was first seen

bool inList(const std::vector<NimBLEAddress> &l, const NimBLEAddress &a) {
  for (auto &x : l) if (x == a) return true;
  return false;
}

void snapshotTrusted() {
  trusted.clear();
  for (int i = 0; i < NimBLEDevice::getNumBonds(); i++) trusted.push_back(NimBLEDevice::getBondedAddress(i));
}

// The library puts the name in the advertising packet, where it doesn't fit next
// to flags + appearance + HID UUID (31-byte limit), so phones hid the nameless pad.
// The name goes in the scan response instead, like a real Xbox pad.
// TRUSTED: not discoverable (missing from scan lists; paired devices still reconnect).
// PAIRING: discoverable, plus Microsoft Swift Pair data so Windows pops up "connect?".
// NimBLE internals used for pairing-mode privacy (plain C functions in the library).
extern "C" {
int ble_hs_pvcy_set_our_irk(const uint8_t *irk);
int ble_hs_pvcy_add_entry(const uint8_t *addr, uint8_t addrtype, const uint8_t *irk);
}

// The controller's resolving list holds saved devices' keys (phones use changing
// private addresses). Setting our key clears it, so put them back.
static int readdPeer(int, union ble_store_value *v, void *) {
  const ble_store_value_sec &sec = v->sec;
  if (sec.irk_present) ble_hs_pvcy_add_entry(sec.peer_addr.val, sec.peer_addr.type, sec.irk);
  return 0;
}

// While pairing, the pad advertises a resolvable private address made from a key
// (IRK) that is new for every pairing session. Saved devices hold older keys, so
// they can't recognise it and don't keep trying to reconnect. A device that pairs
// gets this session's key plus the pad's real public address as its identity, so
// it reconnects to the normal public address in TRUSTED mode.
void newPairingKey() {
  uint8_t irk[16];
  esp_fill_random(irk, sizeof irk);
  ble_hs_pvcy_set_our_irk(irk);
  ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, readdPeer, nullptr);
}

void applyAdvertising() {
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  adv->stop();
  // Saved devices auto-connect to the public address the moment they see it.
  // Advertising from a private address in PAIRING mode keeps them from even trying,
  // instead of connecting and being dropped every half second.
  if (pairingMode) newPairingKey();
  NimBLEDevice::setOwnAddrType(pairingMode ? BLE_OWN_ADDR_RPA_PUBLIC_DEFAULT : BLE_OWN_ADDR_PUBLIC);
  adv->setDiscoverableMode(pairingMode ? BLE_GAP_DISC_MODE_GEN : BLE_GAP_DISC_MODE_NON);
  adv->enableScanResponse(true);
  NimBLEAdvertisementData d, r;
  d.setFlags(pairingMode ? (BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP) : BLE_HS_ADV_F_BREDR_UNSUP);
  d.setAppearance(0x03C4);                 // gamepad
  d.addServiceUUID(NimBLEUUID((uint16_t)0x1812));
  if (pairingMode) {
    const uint8_t swiftPair[] = { 0x06, 0x00, 0x03, 0x00, 0x80 };   // Microsoft, Swift Pair beacon
    d.setManufacturerData(swiftPair, sizeof swiftPair);
  }
  r.setName(PAD_NAME);
  adv->setAdvertisementData(d);
  adv->setScanResponseData(r);
  if (NimBLEDevice::getServer()->getConnectedCount() == 0) adv->start();
  // (while connected, the server restarts advertising with this data on disconnect)
}

void startBle() {
  if (bleStarted) return;
  bleStarted = true;
  bcfg.setGamepadMode(GamepadMode::XInputSeriesX);
  bcfg.setAutoReport(false);
  bleGamepad.begin(&bcfg);
  // begin() sets up security and advertising on its own task: wait for it, then
  // take advertising over (see applyAdvertising).
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  for (uint32_t t0 = millis(); !adv->isAdvertising() && millis() - t0 < 3000; ) delay(10);
  NimBLEDevice::setDeviceName(PAD_NAME);   // GAP name that hosts read after connecting
  // Hand our identity address (and IRK) to devices that pair, and take theirs.
  NimBLEDevice::setSecurityInitKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
  NimBLEDevice::setSecurityRespKey(BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID);
}

// WIRED: drop every Bluetooth link and stop advertising. The radio stack stays
// loaded once started (restarting it is slow), it just goes silent.
void setWired() {
  wiredMode = true;
  pairingMode = false;
  if (bleStarted) {
    NimBLEServer *srv = NimBLEDevice::getServer();
    srv->advertiseOnDisconnect(false);
    NimBLEDevice::getAdvertising()->stop();
    for (uint16_t h : srv->getPeerDevices()) srv->disconnect(h);
  }
  Serial.println("LINK WIRED");
}

void setPairing(bool on) {
  wiredMode = false;
  startBle();
  NimBLEServer *srv = NimBLEDevice::getServer();
  srv->advertiseOnDisconnect(true);
  pairingMode = on;
  pairingSince = millis();
  excluded.clear();
  if (on) {
    for (uint16_t h : srv->getPeerDevices()) {
      excluded.push_back(srv->getPeerInfoByHandle(h).getIdAddress());
      srv->disconnect(h);
    }
  } else {
    snapshotTrusted();
  }
  // Only save new devices in PAIRING mode, so a stranger trying in TRUSTED mode
  // can't push a trusted device out of the 3 slots. Already-paired devices
  // re-encrypt with their saved keys either way.
  NimBLEDevice::setSecurityAuth(on, false, false);
  applyAdvertising();
  Serial.printf("LINK %s (%d saved)\n", on ? "PAIRING" : "TRUSTED", NimBLEDevice::getNumBonds());
}

// Checked every 100 ms: drop connections the current mode doesn't allow.
void policeLinks() {
  static uint32_t last;
  if (millis() - last < 100) return;
  last = millis();
  if (!bleStarted) return;
  NimBLEServer *srv = NimBLEDevice::getServer();
  std::vector<uint16_t> peers = srv->getPeerDevices();
  for (auto it = connSince.begin(); it != connSince.end(); )
    it = std::find(peers.begin(), peers.end(), it->first) == peers.end() ? connSince.erase(it) : std::next(it);
  for (uint16_t h : peers) {
    if (!connSince.count(h)) connSince[h] = millis();
    NimBLEConnInfo ci = srv->getPeerInfoByHandle(h);
    NimBLEAddress id = ci.getIdAddress();   // real identity once encrypted (phones use random addresses)
    const char *why = nullptr;
    if (wiredMode) {
      why = "wired mode";
    } else if (pairingMode) {
      if (inList(excluded, id)) why = "device being switched away from";
    } else if (ci.isEncrypted()) {
      if (!ci.isBonded() || !inList(trusted, id)) why = "not a saved device";
    } else if (millis() - connSince[h] > ENCRYPT_WAIT_MS) {
      why = "never encrypted";
    }
    if (why) {
      Serial.printf("LINK drop %s: %s\n", id.toString().c_str(), why);
      srv->disconnect(h);
    }
  }
  if (pairingMode && peers.empty() && millis() - pairingSince > PAIR_TIMEOUT_MS) {
    Serial.println("LINK pairing timed out");
    setPairing(false);
  }
}

void updateLed() {
  uint32_t t = millis();
  bool on;
  if (wiredMode) on = true;                                                          // steady
  else if (pairingMode) on = bleGamepad.isConnected() ? (t / 500) % 2 : (t / 120) % 2;   // fast blink; slower once paired
  else on = !bleGamepad.isConnected() && (t % 2000) < 60;                            // blip while waiting
  digitalWrite(PIN_LED, on ? LOW : HIGH);
}

void setup() {
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);   // never let debug printing stall the pad if nobody is reading USB
  pinMode(PIN_LOAD, OUTPUT); digitalWrite(PIN_LOAD, HIGH);
  pinMode(PIN_CLK, OUTPUT);  digitalWrite(PIN_CLK, LOW);
  pinMode(PIN_DATA, INPUT);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);

  prefs.begin("qpad", false);
  setDefaults();
  if (prefs.getBytesLength("cfg") == sizeof conf) {
    Config c;
    prefs.getBytes("cfg", &c, sizeof c);
    if (c.ver == CFG_VER) { conf = c; swapMask = prefs.getUChar("swap", 0); }
  }
  applyModePin();
  delay(200);
  measureCentres();   // sticks untouched at power-on
  stuckRaw = readShift();
  modeStuck = MODE_DISABLED ? false : readModeRaw();

  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  // Start mode: powered on from a computer's USB = WIRED, otherwise Bluetooth
  // TRUSTED. A computer sends USB frames within a few ms; a phone charger or the
  // battery doesn't, so those start on Bluetooth.
  bool usbHost = false;
  for (uint32_t t0 = millis(); millis() - t0 < 1500 && !(usbHost = HWCDC::isPlugged()); ) delay(10);
  if (usbHost) setWired(); else setPairing(false);
}

// Next mode for the L1 + R1 + Mode combo.
void nextLinkMode() {
  if (wiredMode) setPairing(false);                  // WIRED   -> TRUSTED
  else if (!pairingMode) setPairing(true);           // TRUSTED -> PAIRING
  else if (HWCDC::isPlugged()) setWired();           // PAIRING -> WIRED (USB only)
  else setPairing(false);                            // PAIRING -> TRUSTED (on battery)
}

// WIRED output: one text line per change (and every 100 ms as a heartbeat) for
// tools/usb_pad.py. Bits: 0 A, 1 B, 2 X, 3 Y, 4 LB, 5 RB, 6 LS, 7 RS, 8 View,
// 9 Menu, 10 Guide, 11 Up, 12 Down, 13 Left, 14 Right. Sticks -32767..32767
// (same values as over Bluetooth), triggers 0..255.
void sendWired(const bool *st, bool modeOut) {
  uint16_t bits = 0;
  for (int b = SOUTH; b < NONE; b++) {
    uint8_t h = hidButton((Btn)b);
    if (h && st[b]) bits |= 1u << (h - 1);
  }
  if (modeOut) bits |= 1u << 10;
  if (st[UP]) bits |= 1u << 11;
  if (st[DOWN]) bits |= 1u << 12;
  if (st[LEFT]) bits |= 1u << 13;
  if (st[RIGHT]) bits |= 1u << 14;
  int v[4];
  readSticks(v);
  char line[64];
  snprintf(line, sizeof line, "G %04X %d %d %d %d %d %d\n", bits,
           xAxis(v[0], 0), xAxis(v[1], 1), xAxis(v[2], 2), xAxis(v[3], 3),
           st[L2] ? 255 : 0, st[R2] ? 255 : 0);
  static char lastLine[64];
  static uint32_t lastSent;
  if (strcmp(line, lastLine) || millis() - lastSent >= 100) {
    Serial.print(line);
    strcpy(lastLine, line);
    lastSent = millis();
  }
}

void loop() {
  pollSerial();
  uint16_t raw = readShift();
  bool mode = readMode();

  // Debug: report every raw input change so wiring can be checked by pressing buttons.
  static uint16_t lastRaw;
  static bool lastMode;
  if (raw != lastRaw || mode != lastMode) {
    for (int i = 0; i < 16; i++) {
      uint16_t m = 1u << i;
      if ((raw ^ lastRaw) & m)
        Serial.printf("RAW %2d %s  (mapped %s)\n", i, raw & m ? "DOWN" : "up", BTN_NAME[conf.map[i]]);
    }
    if (mode != lastMode) Serial.printf("MODE %s\n", mode ? "DOWN" : "up");
    lastRaw = raw; lastMode = mode;
  }

  // Full state line for the live tester page (tools/live.py): raw bits, Mode, raw ADC.
  static uint32_t lastState;
  if (millis() - lastState >= 25) {
    lastState = millis();
    int v[4];
    readSticks(v);
    Serial.printf("S %04X %d %d %d %d %d\n", raw, mode, v[0], v[1], v[2], v[3]);
  }

  bool st[NONE + 1] = {};
  stuckRaw &= raw;                         // released once -> trusted from now on
  if (!mode) modeStuck = false;
  uint16_t good = raw & ~stuckRaw;
  for (int i = 0; i < 16; i++) if (good & (1u << i)) st[conf.map[i]] = true;
  bool modeOut = mode && !modeStuck;
  // L3 + R3 held together for a second acts as Mode (and stops sending L3/R3).
  static uint32_t comboStart = 0;
  if (st[L3] && st[R3]) {
    if (!comboStart) comboStart = millis() | 1;
    if (millis() - comboStart >= COMBO_MS) { modeOut = true; st[L3] = st[R3] = false; }
  } else comboStart = 0;

  // L1 + R1 + Mode held 3 s: next link mode (works connected or not).
  static uint32_t linkStart = 0;
  static bool linkDone = false;
  if (st[L1] && st[R1] && modeOut) {
    if (!linkStart) linkStart = millis() | 1;
    if (!linkDone && millis() - linkStart >= PAIR_HOLD_MS) { linkDone = true; nextLinkMode(); }
  } else { linkStart = 0; linkDone = false; }
  if (st[L1] && st[R1]) modeOut = false;   // don't send Xbox/Guide while doing the combo

  // Cable pulled while WIRED: the pad would be dead on battery, so go to Bluetooth.
  // isPlugged() reads false for long stretches on a healthy link, so it only counts
  // when the computer has also gone quiet: usb_pad.py sends a keep-alive line
  // every 0.5 s (any received byte resets lastRx).
  static uint32_t unpluggedSince = 0;
  if (wiredMode && !HWCDC::isPlugged() && millis() - lastRx > 3000) {
    if (!unpluggedSince) unpluggedSince = millis() | 1;
    if (millis() - unpluggedSince > 3000) { Serial.println("LINK USB unplugged"); setPairing(false); }
  } else unpluggedSince = 0;

  policeLinks();
  updateLed();

  if (wiredMode) {
    sendWired(st, modeOut);
  } else if (bleStarted && bleGamepad.isConnected()) {
    for (int b = SOUTH; b < NONE; b++) {
      uint8_t h = hidButton((Btn)b);
      if (!h) continue;
      if (st[b]) bleGamepad.press(h); else bleGamepad.release(h);
    }
    if (modeOut) bleGamepad.press(HID_MODE); else bleGamepad.release(HID_MODE);
    bleGamepad.setHat1(hatFrom(st[UP], st[DOWN], st[LEFT], st[RIGHT]));
    int v[4];
    readSticks(v);
    bleGamepad.setLeftThumb(xAxis(v[0], 0), xAxis(v[1], 1));
    bleGamepad.setRightThumb(xAxis(v[2], 2), xAxis(v[3], 3));
    bleGamepad.setTriggers(st[L2] ? 32767 : 0, st[R2] ? 32767 : 0);
    bleGamepad.sendReport();
  }
  delay(8);
}
