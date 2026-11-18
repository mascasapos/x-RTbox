// Senal cuadrada de 1 kHz en el pin 18 (y su medida en el pin 4: une 18 y 4 con un cable)
const int SALIDA = 18;
const int ENTRADA = 4;

void setup() {
  Serial.begin(115200);
  // pin, frecuencia (Hz), ciclo de trabajo (%) -> la genera el hardware (LEDC), sin fallos
  float f = signalSquare(SALIDA, 1000, 50);
  Serial.printf("Generando %.1f Hz en el pin %d\n", f, SALIDA);
}

void loop() {
  // cuenta los flancos durante 1000 ms
  float medida = frequencyRead(ENTRADA, 1000);
  Serial.printf("Medido en el pin %d: %.1f Hz\n", ENTRADA, medida);
}
