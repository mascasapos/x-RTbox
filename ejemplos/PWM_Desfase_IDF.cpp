// Tres PWM de 1 kHz desfasadas 120 grados (pines 18, 19 y 21) con hpoint de ESP-IDF
#include "driver/ledc.h"

const int PINES[3] = {18, 19, 21};
const int PERIODO = 1024;        // 10 bits
const int DUTY = 256;            // 25 % (hpoint + duty no debe pasar de 1024)

void setup() {
  ledc_timer_config_t timer = {
    .speed_mode      = LEDC_LOW_SPEED_MODE,
    .duty_resolution = LEDC_TIMER_10_BIT,
    .timer_num       = LEDC_TIMER_0,
    .freq_hz         = 1000,
    .clk_cfg         = LEDC_AUTO_CLK
  };
  ESP_ERROR_CHECK(ledc_timer_config(&timer));

  for (int i = 0; i < 3; i++) {
    ledc_channel_config_t ch = {};
    ch.gpio_num   = PINES[i];
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ch.channel    = (ledc_channel_t)i;
    ch.timer_sel  = LEDC_TIMER_0;
    ch.duty       = DUTY;
    ch.hpoint     = i * PERIODO / 3;   // 0, 341 y 682 = 0, 120 y 240 grados
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
  }
  Serial.printf("PWM a %lu Hz en 3 fases\n", ledc_get_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_0));
}

void loop() {
}
