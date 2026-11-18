// Senoidal de 1 kHz por el DAC (pin 25) y triangular por el pin 26 (ESP32 clasico)
// En otros pines, o en ESP32-S3/C3/C6, sale por sigma-delta: pon un filtro RC
// (resistencia de 1 kOhm en serie y condensador de 47 nF a GND) para verla suave.

void setup() {
  Serial.begin(115200);
  // pin, frecuencia (Hz), amplitud (V de pico), nivel medio (V)
  float f = signalSine(25, 1000, 1.5, 1.65);
  signalTriangle(26, 1000, 1.5, 1.65);
  Serial.printf("Senoidal de %.3f Hz en el pin 25 y triangular en el 26\n", f);
  // tambien: signalSawtooth(pin, Hz) y signalStop(pin)
}

void loop() {
  delay(1000);
}
