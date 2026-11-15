// Quantum Pad: ESP32-C3 SuperMini BLE gamepad.
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
#include <Arduino.h>
#include <BleGamepad.h>
#include <Preferences.h>

const int PIN_LX = 0, PIN_LY = 1, PIN_RY = 3, PIN_RX = 4;
const int PIN_LOAD = 5, PIN_CLK = 6, PIN_DATA = 7;
const int PIN_MODE = 20;

enum Btn : uint8_t { UP, DOWN, LEFT, RIGHT, SOUTH, EAST, NORTH, WEST,
                     L1, R1, L2, R2, SELECT, START, L3, R3, NONE };
const char *BTN_NAME[] = { "UP", "DOWN", "LEFT", "RIGHT", "X(south)", "O(east)", "TRI(north)", "SQ(west)",
                           "L1", "R1", "L2", "R2", "SELECT", "START", "L3", "R3", "-" };
// Default wiring: raw shift order is chip1 H..A, then chip2 H..A.
const uint8_t DEFAULT_MAP[16] = { R3, L3, START, SELECT, RIGHT, LEFT, DOWN, UP,
                                  R2, L2, R1, L1, WEST, SOUTH, EAST, NORTH };

uint8_t hidButton(Btn b) {
  switch (b) {
    case SOUTH: return 1;  case EAST: return 2;  case NORTH: return 4;  case WEST: return 5;
    case L1: return 7;     case R1: return 8;    case L2: return 9;     case R2: return 10;
    case SELECT: return 11; case START: return 12; case L3: return 14;  case R3: return 15;
    default: return 0;
  }
}
const uint8_t HID_MODE = 13;

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

BleGamepad bleGamepad("Quantum Pad", "DIY", 100);
BleGamepadConfiguration bcfg;
int centre[4];
const int stickPin[4] = { PIN_LX, PIN_LY, PIN_RX, PIN_RY };

void setDefaults() {
  conf.ver = CFG_VER;
  memcpy(conf.map, DEFAULT_MAP, 16);
  for (int a = 0; a < 4; a++) { conf.amin[a] = 0; conf.actr[a] = -1; conf.amax[a] = 4095; }
  conf.dz = 120; conf.inv = 0; conf.modePol = 0;
  swapMask = 0;
}

void applyModePin() { pinMode(PIN_MODE, conf.modePol ? INPUT_PULLDOWN : INPUT_PULLUP); }

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
  return bits;
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
  if (!strcmp(l, "SAVE")) { prefs.putBytes("cfg", &conf, sizeof conf); prefs.putUChar("swap", swapMask); Serial.println("OK SAVE"); printCfg(); return; }
  if (!strcmp(l, "DEFAULTS")) { setDefaults(); applyModePin(); measureCentres(); Serial.println("OK DEFAULTS"); printCfg(); return; }
  Serial.printf("ERR unknown: %s\n", l);
}

void pollSerial() {
  static char buf[160];
  static int n = 0;
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') { buf[n] = 0; if (n) handleCmd(buf); n = 0; }
    else if (n < (int)sizeof buf - 1) buf[n++] = ch;
  }
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
  modeStuck = conf.modePol ? digitalRead(PIN_MODE) : !digitalRead(PIN_MODE);

  bcfg.setAutoReport(false);
  bcfg.setButtonCount(16);
  bcfg.setHatSwitchCount(1);
  bcfg.setAxesMin(0);
  bcfg.setAxesMax(32767);
  bleGamepad.begin(&bcfg);
}

void loop() {
  pollSerial();
  uint16_t raw = readShift();
  bool mode = conf.modePol ? digitalRead(PIN_MODE) : !digitalRead(PIN_MODE);

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

  if (bleGamepad.isConnected()) {
    bool st[NONE + 1] = {};
    stuckRaw &= raw;                       // released once -> trusted from now on
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
    for (int b = SOUTH; b < NONE; b++) {
      uint8_t h = hidButton((Btn)b);
      if (!h) continue;
      if (st[b]) bleGamepad.press(h); else bleGamepad.release(h);
    }
    if (modeOut) bleGamepad.press(HID_MODE); else bleGamepad.release(HID_MODE);
    bleGamepad.setHat1(hatFrom(st[UP], st[DOWN], st[LEFT], st[RIGHT]));
    int v[4];
    readSticks(v);
    bleGamepad.setLeftThumb(toAxis(v[0], 0), toAxis(v[1], 1));
    bleGamepad.setRightThumb(toAxis(v[2], 2), toAxis(v[3], 3));
    bleGamepad.sendReport();
  }
  delay(8);
}
