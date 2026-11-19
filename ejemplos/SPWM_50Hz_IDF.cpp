// SPWM de 50 Hz: portadora LEDC de 20 kHz en los pines 19 (+) y 21 (-), duty desde tablas senoidales
// La interrupcion alTick() la reconoce X-RTbox y la ejecuta el hardware (20000 veces por segundo).
#include "driver/ledc.h"

#define PIN_A 19                 // rama A (PWM +)
#define PIN_B 21                 // rama B (PWM -)

#define F_PORTADORA 20000        // Hz
#define F_SALIDA    50           // Hz
#define N_MUESTRAS  (F_PORTADORA / F_SALIDA)   // 400
#define RES_BITS    LEDC_TIMER_10_BIT
#define DUTY_MAX    1023
#define INDICE_M    0.90f        // índice de modulación (0..1)

static uint16_t tablaA[N_MUESTRAS];
static uint16_t tablaB[N_MUESTRAS];
static volatile uint16_t idx = 0;
hw_timer_t *timerMuestreo = nullptr;

void IRAM_ATTR alTick() {
  ledc_set_duty_and_update(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, tablaA[idx], 0);
  ledc_set_duty_and_update(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_1, tablaB[idx], 0);
  if (++idx >= N_MUESTRAS) idx = 0;
}

void configurarCanal(int pin, ledc_channel_t canal) {
  ledc_channel_config_t ch = {};
  ch.gpio_num   = pin;
  ch.speed_mode = LEDC_HIGH_SPEED_MODE;
  ch.channel    = canal;
  ch.timer_sel  = LEDC_TIMER_0;
  ch.duty       = 512;
  ch.hpoint     = 0;
  ledc_channel_config(&ch);
}

void setup() {
  // Tablas senoidales
  for (int i = 0; i < N_MUESTRAS; i++) {
    float s = sinf(2.0f * PI * i / N_MUESTRAS);
    tablaA[i] = (uint16_t)(512 + 512 * INDICE_M * s);
    tablaB[i] = (uint16_t)(512 - 512 * INDICE_M * s);
  }

  // Portadora PWM: 20 kHz, 10 bits (divisor exacto, sin error de frecuencia)
  ledc_timer_config_t timer = {};
  timer.speed_mode      = LEDC_HIGH_SPEED_MODE;
  timer.duty_resolution = RES_BITS;
  timer.timer_num       = LEDC_TIMER_0;
  timer.freq_hz         = F_PORTADORA;
  timer.clk_cfg         = LEDC_AUTO_CLK;
  ledc_timer_config(&timer);

  configurarCanal(PIN_A, LEDC_CHANNEL_0);
  configurarCanal(PIN_B, LEDC_CHANNEL_1);

  // Timer de muestreo: actualiza el duty cada periodo de portadora (20 kHz)
  timerMuestreo = timerBegin(1000000);              // base 1 MHz
  timerAttachInterrupt(timerMuestreo, &alTick);
  timerAlarm(timerMuestreo, 1000000 / F_PORTADORA, true, 0);   // cada 50 µs
}

void loop() {
  // Todo lo hace el hardware y la interrupción
}
