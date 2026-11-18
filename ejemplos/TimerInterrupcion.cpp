// Temporizador por hardware a 1 kHz: cuenta interrupciones y hace parpadear el LED
hw_timer_t *timer = NULL;
volatile uint32_t ticks = 0;
const int LED = 2;
bool encendido = false;

void ARDUINO_ISR_ATTR onTimer() {
  ticks++;
  if (ticks % 500 == 0) {        // cada 500 ms
    encendido = !encendido;
    digitalWrite(LED, encendido);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  timer = timerBegin(1000000);              // el temporizador cuenta a 1 MHz
  timerAttachInterrupt(timer, &onTimer);
  timerAlarm(timer, 1000, true, 0);         // alarma cada 1000 us = 1 kHz
}

void loop() {
  delay(1000);
  Serial.printf("Interrupciones: %u\n", ticks);
}
