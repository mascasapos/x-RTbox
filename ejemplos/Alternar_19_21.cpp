// Pines 19 y 21 alternando cada 500 ms (mira las dos senales en el OSCILOSCOPIO a 200 ms/div)
void setup() {
  pinMode(19, OUTPUT);
  pinMode(21, OUTPUT);
}

void loop() {
  digitalWrite(19, HIGH);
  digitalWrite(21, LOW);
  delay(500);
  digitalWrite(19, LOW);
  digitalWrite(21, HIGH);
  delay(500);
}
