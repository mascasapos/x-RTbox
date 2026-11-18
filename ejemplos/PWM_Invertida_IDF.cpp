// Dos PWM de 1 kHz complementarias (pin 19 normal, pin 21 invertida) con la API de ESP-IDF
#include "driver/ledc.h"

#define PIN_A 19   // senal normal
#define PIN_B 21   // senal invertida de D19

void configurarCanal(int pin, ledc_channel_t canal, bool invertir) {
  ledc_channel_config_t ch = {};
  ch.gpio_num   = pin;
  ch.speed_mode = LEDC_LOW_SPEED_MODE;   // en el ESP32 clasico tambien vale LEDC_HIGH_SPEED_MODE
  ch.channel    = canal;
  ch.timer_sel  = LEDC_TIMER_0;
  ch.duty       = 512;                   // 50 % (de 1024)
  ch.hpoint     = 0;
  ch.flags.output_invert = invertir ? 1 : 0;
  ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

void setup() {
  // Timer: 1 kHz, resolucion de 10 bits
  ledc_timer_config_t timer = {};
  timer.speed_mode      = LEDC_LOW_SPEED_MODE;
  timer.duty_resolution = LEDC_TIMER_10_BIT;
  timer.timer_num       = LEDC_TIMER_0;
  timer.freq_hz         = 1000;
  timer.clk_cfg         = LEDC_AUTO_CLK;
  ESP_ERROR_CHECK(ledc_timer_config(&timer));

  configurarCanal(PIN_A, LEDC_CHANNEL_0, false);
  configurarCanal(PIN_B, LEDC_CHANNEL_1, true);
}

void loop() {
  // Todo lo genera el hardware
}
