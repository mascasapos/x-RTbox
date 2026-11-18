// Servo en el pin 13 que barre de 0 a 180 grados (alimentalo con 5 V aparte)
#include <ESP32Servo.h>
Servo servo;

void setup() {
  servo.attach(13);          // tambien: servo.attach(13, 500, 2400) con los limites en us
}

void loop() {
  for (int a = 0; a <= 180; a += 2) {
    servo.write(a);
    delay(15);
  }
  for (int a = 180; a >= 0; a -= 2) {
    servo.write(a);
    delay(15);
  }
}
