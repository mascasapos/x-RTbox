// Cuenta pulsos con attachInterrupt en el pin 4 (un sensor, o un boton a GND)
const int ENTRADA = 4;
volatile int pulsos = 0;

void IRAM_ATTR alPulso() {
  pulsos++;
}

void setup() {
  Serial.begin(115200);
  pinMode(ENTRADA, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENTRADA), alPulso, FALLING);
}

void loop() {
  delay(1000);
  noInterrupts();
  int n = pulsos;
  pulsos = 0;
  interrupts();
  Serial.printf("%d pulsos por segundo\n", n);
}
