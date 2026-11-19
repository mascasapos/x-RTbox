// Secuencia de 100 pasos en los pines 19 y 21 reproducida por hardware (ciclo de 60 Hz)
// Cada numero de la secuencia dice que pines se encienden: bit 0 = primer pin, bit 1 = segundo
//   0 = los dos apagados, 1 = solo el pin 19, 2 = solo el pin 21  (3 seria los dos)
// Los dos pines cambian a la vez, con una sola escritura en el registro del GPIO.

const int pines[] = {19, 21};

int secuencia[] = {
  1,0,0,0,0,1,1,0,0,0,1,1,1,0,0,1,1,1,1,0,
  1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,1,1,0,0,
  1,1,0,0,0,1,0,0,0,0,2,0,0,0,0,2,2,0,0,0,
  2,2,2,0,0,2,2,2,2,0,2,2,2,2,2,2,2,2,2,2,
  2,2,2,2,0,2,2,2,0,0,2,2,0,0,0,2,0,0,0,0
};
const int longitud = sizeof(secuencia) / sizeof(secuencia[0]);

const float FRECUENCIA = 60.0;   // Hz del ciclo completo (los 100 pasos)

void setup() {
  Serial.begin(115200);
  // pines 4, 5 y 18 siempre encendidos (3.3 V)
  pinMode(4, OUTPUT);  digitalWrite(4, HIGH);
  pinMode(5, OUTPUT);  digitalWrite(5, HIGH);
  pinMode(18, OUTPUT); digitalWrite(18, HIGH);

  // pines, cuantos pines, secuencia, cuantos pasos, pasos por segundo
  float pasos = patternStart(pines, 2, secuencia, longitud, FRECUENCIA * longitud);
  Serial.printf("%.1f pasos/s -> ciclo de %.3f Hz\n", pasos, pasos / longitud);
  // para cambiar la velocidad en marcha: patternSetFrequency(pasosPorSegundo);  para parar: patternStop();
}

void loop() {
  // la secuencia la reproduce el hardware: aqui puedes hacer otras cosas
}
