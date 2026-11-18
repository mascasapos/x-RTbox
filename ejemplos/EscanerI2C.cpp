// Busca dispositivos I2C conectados (pantallas, sensores...). Pines por defecto: SDA y SCL
#include <Wire.h>

void setup() {
  Serial.begin(115200);
  Wire.begin(SDA, SCL);      // o Wire.begin(21, 22) con los pines que quieras
  Serial.printf("Buscando en SDA=%d SCL=%d...\n", SDA, SCL);
}

void loop() {
  int encontrados = 0;
  for (byte dir = 1; dir < 127; dir++) {
    Wire.beginTransmission(dir);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  dispositivo en 0x%02X\n", dir);
      encontrados++;
    }
  }
  Serial.printf("%d dispositivo(s)\n\n", encontrados);
  delay(5000);
}
