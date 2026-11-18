// Luz que va y viene por 6 LEDs
int leds[] = {13, 12, 14, 27, 26, 25};
const int N = sizeof(leds) / sizeof(leds[0]);

void setup() {
  for (int i = 0; i < N; i++) pinMode(leds[i], OUTPUT);
}

void paso(int i) {
  digitalWrite(leds[i], HIGH);
  delay(80);
  digitalWrite(leds[i], LOW);
}

void loop() {
  for (int i = 0; i < N; i++) paso(i);
  for (int i = N - 2; i > 0; i--) paso(i);
}
