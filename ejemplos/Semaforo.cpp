// Semaforo con tres LEDs (con resistencia de 220 ohm cada uno)
#define ROJO 25
#define AMBAR 26
#define VERDE 27

void enciende(int pin, unsigned long ms, const char* nombre) {
  digitalWrite(ROJO, LOW);
  digitalWrite(AMBAR, LOW);
  digitalWrite(VERDE, LOW);
  digitalWrite(pin, HIGH);
  Serial.println(nombre);
  delay(ms);
}

void setup() {
  pinMode(ROJO, OUTPUT);
  pinMode(AMBAR, OUTPUT);
  pinMode(VERDE, OUTPUT);
}

void loop() {
  enciende(VERDE, 4000, "Verde");
  enciende(AMBAR, 1500, "Ambar");
  enciende(ROJO, 4000, "Rojo");
}
