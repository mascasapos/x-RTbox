// SPWM de 50 Hz con las funciones de tablas de X-RTbox (sin interrupcion propia)
// Portadora de 20 kHz en los pines 19 y 21; cada 50 us el hardware pone en cada canal
// la muestra siguiente de su tabla (400 muestras = 1 ciclo de 50 Hz).

const int PIN_A = 19;
const int PIN_B = 21;
const int F_PORTADORA = 20000;
const int F_SALIDA = 50;
const int N = F_PORTADORA / F_SALIDA;   // 400 muestras
const float INDICE_M = 0.90;            // indice de modulacion (0..1)

uint16_t tablaA[400];
uint16_t tablaB[400];

void setup() {
  Serial.begin(115200);
  for (int i = 0; i < N; i++) {
    float s = sin(2 * PI * i / N);
    tablaA[i] = 512 + 512 * INDICE_M * s;
    tablaB[i] = 512 - 512 * INDICE_M * s;
  }
  ledcAttach(PIN_A, F_PORTADORA, 10);   // los dos con la misma frecuencia y bits comparten temporizador
  ledcAttach(PIN_B, F_PORTADORA, 10);

  pwmTableAddPin(PIN_A, tablaA, N);     // o pwmTableAdd(modo, canal, tabla, N) con ledc_channel_config
  pwmTableAddPin(PIN_B, tablaB, N);
  float hz = pwmTableStart(F_PORTADORA);   // muestras por segundo
  Serial.printf("SPWM: %.1f muestras/s -> %.3f Hz de salida\n", hz, hz / N);
  // en marcha: pwmTableSetRate(muestrasPorSegundo) cambia la frecuencia de salida; pwmTableStop() para
}

void loop() {
}
