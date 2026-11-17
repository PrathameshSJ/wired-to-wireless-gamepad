// Slow pin wiggle for multimeter checks: LOAD (GPIO5) and CLK (GPIO6) swap
// between 0 V and 3.3 V every 3 s. Also prints what DATA (GPIO7) reads.
#include <Arduino.h>
const int PIN_LOAD = 5, PIN_CLK = 6, PIN_DATA = 7;
void setup() {
  Serial.begin(115200);
  pinMode(PIN_LOAD, OUTPUT); pinMode(PIN_CLK, OUTPUT);
  pinMode(PIN_DATA, INPUT_PULLDOWN);
}
void loop() {
  static bool phase;
  phase = !phase;
  digitalWrite(PIN_LOAD, phase ? HIGH : LOW);
  digitalWrite(PIN_CLK, phase ? LOW : HIGH);
  Serial.printf("LOAD=%s CLK=%s  DATA reads %d\n", phase ? "3.3V" : "0V", phase ? "0V" : "3.3V", digitalRead(PIN_DATA));
  delay(3000);
}
