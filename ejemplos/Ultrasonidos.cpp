// Sensor de distancia HC-SR04: TRIG en el pin 5, ECHO en el pin 18
// (el ECHO da 5V: usa un divisor de tension para bajarlo a 3.3V)
const int TRIG = 5;
const int ECHO = 18;

void setup() {
  Serial.begin(115200);
  pinMode(TRIG, OUTPUT);
  pinMode(ECHO, INPUT);
}

float distanciaCm() {
  digitalWrite(TRIG, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG, LOW);
  unsigned long us = pulseIn(ECHO, HIGH, 30000);
  if (us == 0) return -1;
  return us * 0.0343 / 2;
}

void loop() {
  float d = distanciaCm();
  if (d < 0) Serial.println("Sin eco (fuera de rango)");
  else {
    Serial.print("Distancia: ");
    Serial.print(d, 1);
    Serial.println(" cm");
  }
  delay(250);
}
