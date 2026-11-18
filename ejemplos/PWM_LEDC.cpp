// PWM de 1 kHz con ledc (core 3.x): el brillo del LED del pin 2 sube y baja
const int PIN = 2;
const int BITS = 10;   // valores de 0 a 1023

void setup() {
  ledcAttach(PIN, 1000, BITS);   // pin, frecuencia (Hz), resolucion (bits)
}

void loop() {
  for (int d = 0; d <= 1023; d += 8) {
    ledcWrite(PIN, d);
    delay(5);
  }
  for (int d = 1023; d >= 0; d -= 8) {
    ledcWrite(PIN, d);
    delay(5);
  }
}
