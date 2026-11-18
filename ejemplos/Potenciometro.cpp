// Lee un potenciometro en el pin 34 y regula el brillo de un LED en el pin 2
const int POT = 34;
const int LED = 2;

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
}

void loop() {
  int lectura = analogRead(POT);            // 0 .. 4095
  int brillo = map(lectura, 0, 4095, 0, 255);
  analogWrite(LED, brillo);
  float voltios = lectura * 3.3 / 4095;
  Serial.printf("Lectura: %4d  ->  %.2f V  ->  brillo %d\n", lectura, voltios, brillo);
  delay(300);
}
