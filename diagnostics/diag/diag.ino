// Wiring diagnostic: reads the 74HC165 chain with the DATA pin pulled down,
// then pulled up. If the result follows the pull, nothing is driving DATA.
#include <Arduino.h>
const int PIN_LOAD = 5, PIN_CLK = 6, PIN_DATA = 7, PIN_MODE = 20;

uint16_t readShift() {
  digitalWrite(PIN_LOAD, LOW); delayMicroseconds(5);
  digitalWrite(PIN_LOAD, HIGH); delayMicroseconds(5);
  uint16_t bits = 0;
  for (int i = 0; i < 16; i++) {
    if (digitalRead(PIN_DATA)) bits |= 1u << i;
    digitalWrite(PIN_CLK, HIGH); delayMicroseconds(5);
    digitalWrite(PIN_CLK, LOW); delayMicroseconds(5);
  }
  return bits;
}

void bin16(uint16_t v) { for (int i = 0; i < 16; i++) Serial.print((v >> i) & 1); }

void setup() {
  Serial.begin(115200);
  pinMode(PIN_LOAD, OUTPUT); digitalWrite(PIN_LOAD, HIGH);
  pinMode(PIN_CLK, OUTPUT); digitalWrite(PIN_CLK, LOW);
  pinMode(PIN_MODE, INPUT_PULLDOWN);
}

void loop() {
  pinMode(PIN_DATA, INPUT_PULLDOWN); delay(2);
  uint16_t dn = readShift(); int idleDn = digitalRead(PIN_DATA);
  pinMode(PIN_DATA, INPUT_PULLUP); delay(2);
  uint16_t up = readShift(); int idleUp = digitalRead(PIN_DATA);
  Serial.print("pulldown: "); bin16(dn); Serial.printf(" idle=%d   pullup: ", idleDn);
  bin16(up); Serial.printf(" idle=%d   mode=%d\n", idleUp, digitalRead(PIN_MODE));
  delay(400);
}
