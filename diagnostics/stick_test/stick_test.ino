// Stage-1 test: right stick on GPIO4 (X) / GPIO3 (Y) as a BLE gamepad.
// Also prints raw readings over USB serial for bench testing.
#include <Arduino.h>
#include <BleGamepad.h>

const int PIN_RX = 4;           // GPIO2 is a strapping pin: a stick on it forces download mode
const int PIN_RY = 3;
const int DEADZONE = 120;          // raw ADC counts (0..4095) ignored around centre

BleGamepad bleGamepad("Quantum Pad", "DIY", 100);
BleGamepadConfiguration cfg;
int centreX, centreY;

int readAvg(int pin) {
  long sum = 0;
  for (int i = 0; i < 8; i++) sum += analogRead(pin);
  return sum / 8;
}

// Map raw 0..4095 to 0..32767 with the calibrated centre at 16384.
int16_t toAxis(int raw, int centre) {
  int d = raw - centre;
  if (abs(d) < DEADZONE) return 16384;
  long v = d < 0 ? 16384L + (long)d * 16384 / centre
                 : 16384L + (long)d * 16383 / (4095 - centre);
  return constrain(v, 0, 32767);
}

void setup() {
  Serial.begin(115200);
  analogReadResolution(12);
  analogSetAttenuation(ADC_11db);  // full 0..3.3 V range
  delay(200);
  centreX = readAvg(PIN_RX);       // keep sticks untouched at power-on
  centreY = readAvg(PIN_RY);

  cfg.setAutoReport(false);
  cfg.setButtonCount(16);
  cfg.setHatSwitchCount(1);
  cfg.setAxesMin(0);
  cfg.setAxesMax(32767);
  bleGamepad.begin(&cfg);
}

void loop() {
  int rx = readAvg(PIN_RX), ry = readAvg(PIN_RY);
  static uint32_t lastPrint;
  if (millis() - lastPrint > 200) {
    lastPrint = millis();
    Serial.printf("RX=%4d RY=%4d  centre=%d,%d  BT=%s\n", rx, ry, centreX, centreY,
                  bleGamepad.isConnected() ? "connected" : "waiting");
  }
  if (bleGamepad.isConnected()) {
    bleGamepad.setLeftThumb(16384, 16384);
    bleGamepad.setRightThumb(toAxis(rx, centreX), toAxis(ry, centreY));
    bleGamepad.sendReport();
  }
  delay(10);
}
