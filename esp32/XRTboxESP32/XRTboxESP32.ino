// =====================================================================
//  X-RTbox - Firmware para ESP32
//
//  Se sube UNA sola vez (con el Arduino IDE o desde la propia app por
//  USB-C). Despues, los programas se mandan desde la app X-RTbox del movil
//  por Bluetooth (BLE) o por cable USB y se ejecutan al instante con el
//  interprete MiniC, sin recompilar.
//
//  - El ultimo programa recibido se guarda en la memoria (LittleFS) y
//    se vuelve a ejecutar solo al encender el ESP32.
//  - Si mantienes pulsado el boton BOOT al encender, NO se ejecuta el
//    programa guardado (arranque seguro).
//  - Lo que el programa imprime con Serial.print sale por el USB y en
//    la consola de la app.
//
//  Placas: ESP32, ESP32-S3, ESP32-C3, ESP32-C6 (cualquiera con BLE).
//  Core: "esp32" de Espressif 3.x (recomendado 3.3.x) en el Arduino IDE.
//        (La 4.0 cambio la API de Bluetooth y no es compatible.)
//  Menu Herramientas -> Partition Scheme -> "Huge APP (3MB No OTA/1MB SPIFFS)"
// =====================================================================

#include <Arduino.h>
#if __has_include("esp_arduino_version.h")
#include "esp_arduino_version.h"
#endif
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <LittleFS.h>
#include <atomic>
#include <string>
#include <stdarg.h>
#include "soc/soc_caps.h"
#include "driver/gpio.h"
#include "esp_system.h"
#if __has_include("esp_random.h")
#include "esp_random.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_idf_version.h"
#include "esp32-hal-periman.h"
#include "driver/ledc.h"
#include "soc/soc.h"
#include "soc/gpio_reg.h"
#include <Wire.h>
#include <SPI.h>
#include "MiniC.h"
// Version del firmware: la escribe GitHub al compilar (huella del codigo fuente).
// La app la compara con la del firmware que lleva dentro y avisa si el ESP32 tiene uno antiguo.
#if __has_include("xrtbox_version.h")
#include "xrtbox_version.h"
#endif
#ifndef XRTBOX_FW_VERSION
#define XRTBOX_FW_VERSION "dev"
#endif

// Las funciones de PWM, temporizadores, etc. son las del core 3.x.
#if !defined(ESP_ARDUINO_VERSION_MAJOR) || (ESP_ARDUINO_VERSION_MAJOR < 3)
#error "X-RTbox necesita el core esp32 3.x. En Herramientas > Placa > Gestor de placas instala 'esp32' version 3.3.x."
#endif
// La version 4.0 del core reescribio la libreria Bluetooth con otra API.
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 4)
#error "X-RTbox usa la API Bluetooth del core esp32 2.x/3.x. En Herramientas > Placa > Gestor de placas instala 'esp32' version 3.3.x."
#endif
// Descriptor 2902 (CCCD) para las notificaciones, como en los ejemplos
// oficiales. Con NimBLE el core lo crea solo y este no estorba.
#include <BLE2902.h>
#define XRTBOX_ADD_2902 1

// El interprete corre en su propia tarea con una pila grande.
static const uint32_t VM_STACK = 40 * 1024;
#ifndef ARDUINO_RUNNING_CORE
#define ARDUINO_RUNNING_CORE 0
#endif

// ---------------------------------------------------------------------
//  Configuracion (debe coincidir con la app)
// ---------------------------------------------------------------------
#define SERVICE_UUID "a7c40001-ee5b-4fd0-9052-1429fba680b4"
#define RX_UUID      "a7c40002-ee5b-4fd0-9052-1429fba680b4"   // movil -> ESP32 (write)
#define TX_UUID      "a7c40003-ee5b-4fd0-9052-1429fba680b4"   // ESP32 -> movil (notify)

static const char* PROG_FILE = "/xrtbox_programa.cpp";
static const char* NAME_FILE = "/xrtbox_nombre.txt";
static const size_t MAX_PROGRAM = 32 * 1024;   // tamano maximo del codigo

#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32C6 || CONFIG_IDF_TARGET_ESP32H2 || CONFIG_IDF_TARGET_ESP32C2
static const int SAFE_BOOT_PIN = 9;     // boton BOOT
#else
static const int SAFE_BOOT_PIN = 0;     // boton BOOT
#endif

// ---------------------------------------------------------------------
//  Buffer circular protegido (varias tareas)
// ---------------------------------------------------------------------
template <size_t N>
class Ring {
 public:
  size_t write(const char* s, size_t n) {
    size_t w = 0;
    portENTER_CRITICAL(&mux);
    for (; w < n; w++) {
      size_t nh = (head + 1) % N;
      if (nh == tail) { lost = true; break; }
      buf[head] = s[w];
      head = nh;
    }
    portEXIT_CRITICAL(&mux);
    return w;
  }
  size_t read(char* out, size_t max) {
    size_t r = 0;
    portENTER_CRITICAL(&mux);
    while (r < max && tail != head) {
      out[r++] = buf[tail];
      tail = (tail + 1) % N;
    }
    portEXIT_CRITICAL(&mux);
    return r;
  }
  int peek() {
    int c = -1;
    portENTER_CRITICAL(&mux);
    if (tail != head) c = (unsigned char)buf[tail];
    portEXIT_CRITICAL(&mux);
    return c;
  }
  size_t size() {
    portENTER_CRITICAL(&mux);
    size_t s = (head + N - tail) % N;
    portEXIT_CRITICAL(&mux);
    return s;
  }
  void clear() {
    portENTER_CRITICAL(&mux);
    head = tail = 0;
    lost = false;
    portEXIT_CRITICAL(&mux);
  }
  bool takeLost() {
    portENTER_CRITICAL(&mux);
    bool l = lost;
    lost = false;
    portEXIT_CRITICAL(&mux);
    return l;
  }

 private:
  char buf[N];
  size_t head = 0, tail = 0;
  bool lost = false;
  portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
};

static Ring<4096> consoleOut;   // salida del programa -> movil
static Ring<1024> progInput;    // entrada Serial del programa (consola del movil o USB)

// ---------------------------------------------------------------------
//  Estado compartido
// ---------------------------------------------------------------------
enum ProgState { ST_NONE = 0, ST_RUN, ST_STOP, ST_ERR, ST_FIN };
static std::atomic<int> progState(ST_NONE);
static char progName[48] = "";
static portMUX_TYPE nameMux = portMUX_INITIALIZER_UNLOCKED;

static std::atomic<bool> bleConnected(false);
static std::atomic<int> bleMtu(23);
static std::atomic<bool> cmdRun(false), cmdStop(false), cmdInfo(false);

// Recepcion de programas: un estado por cada via (Bluetooth y USB)
struct RxState {
  std::string buf;
  std::string name;
  size_t expected = 0;
  uint32_t crc = 0;
  bool active = false;
};
static RxState bleRx, usbRx;

// La app esta conectada por USB si ha mandado alguna trama hace poco
static std::atomic<uint32_t> lastUsbFrame(0);
static std::atomic<bool> usbSeen(false);
static bool usbAppActive() {
  return usbSeen.load() && (uint32_t)(millis() - lastUsbFrame.load()) < 12000u;
}

// Entrega al loop()
static std::atomic<bool> pendingReady(false);
static std::atomic<bool> pendingClaim(false);
static std::string pendingCode;
static std::string pendingName;

struct CtlMsg {
  char text[200];
};
static QueueHandle_t ctlQueue = nullptr;

static BLEServer* pServer = nullptr;
static BLECharacteristic* pTx = nullptr;
static char devName[24];

// ---------------------------------------------------------------------
//  Utilidades
// ---------------------------------------------------------------------
static uint32_t crc32(const uint8_t* d, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= d[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

static void setProgName(const char* n) {
  portENTER_CRITICAL(&nameMux);
  strncpy(progName, n, sizeof(progName) - 1);
  progName[sizeof(progName) - 1] = 0;
  portEXIT_CRITICAL(&nameMux);
}

static void getProgName(char* out, size_t n) {
  portENTER_CRITICAL(&nameMux);
  strncpy(out, progName, n - 1);
  out[n - 1] = 0;
  portEXIT_CRITICAL(&nameMux);
}

// Mensaje de control hacia el movil: tipo 'K' (ok), '!' (error), 'T' (estado)
static void sendCtl(char type, const char* fmt, ...) {
  CtlMsg m;
  m.text[0] = type;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(m.text + 1, sizeof(m.text) - 1, fmt, ap);
  va_end(ap);
  bool usb = usbAppActive();
  if (!usb) Serial.printf("[X-RTbox] %s\n", m.text);
  if (ctlQueue && (bleConnected.load() || usb)) xQueueSend(ctlQueue, &m, 0);
}

static void sendState() {
  static const char* names[] = {"NONE", "RUN", "STOP", "ERR", "FIN"};
  char n[48];
  getProgName(n, sizeof n);
  int st = progState.load();
  sendCtl('T', "%s|%s|%u|%s", names[st < 0 || st > 4 ? 0 : st], n, (unsigned)ESP.getFreeHeap(), XRTBOX_FW_VERSION);
}

// ---------------------------------------------------------------------
//  Interrupciones, temporizadores y generadores de senal
//  Las interrupciones reales solo anotan el evento; el interprete llama
//  a la funcion del programa justo despues (entre dos instrucciones).
// ---------------------------------------------------------------------
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#include "hal/dac_ll.h"
#define XRT_HAS_DAC 1
#else
#define XRT_HAS_DAC 0
#endif
#if SOC_SDM_SUPPORTED
#include "driver/sdm.h"
#endif
#if (SOC_I2C_NUM > 1) || (defined(SOC_HP_I2C_NUM) && (SOC_HP_I2C_NUM > 1))
#define XRT_HAS_WIRE1 1
#else
#define XRT_HAS_WIRE1 0
#endif
#ifdef SOC_LEDC_SUPPORT_HS_MODE
#define XRT_LEDC_CH (SOC_LEDC_CHANNEL_NUM * 2)
#else
#define XRT_LEDC_CH (SOC_LEDC_CHANNEL_NUM)
#endif
#define XRT_LEDC_BITS ((SOC_LEDC_TIMER_BIT_WIDTH) > 14 ? 14 : (SOC_LEDC_TIMER_BIT_WIDTH))

static portMUX_TYPE evMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t evCount[32];
static volatile uint32_t* evBits = nullptr;   // -> hal.eventBits
static SemaphoreHandle_t evSem = nullptr;     // despierta al interprete si esta en delay()

static void ARDUINO_ISR_ATTR postEvent(int slot) {
  portENTER_CRITICAL_ISR(&evMux);
  evCount[slot] = evCount[slot] + 1;
  *evBits = *evBits | (1u << slot);
  portEXIT_CRITICAL_ISR(&evMux);
  if (evSem) {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(evSem, &woken);
    if (woken == pdTRUE) portYIELD_FROM_ISR();
  }
}

// Interrupciones de pines (attachInterrupt) y contadores de pulsos
struct PinIrq {
  volatile uint32_t count;
  volatile int8_t slot;   // evento del programa (-1 = solo cuenta)
  bool active;
  bool counter;
};
static PinIrq pinIrq[SOC_GPIO_PIN_COUNT];

static void ARDUINO_ISR_ATTR pinIsr(void* arg) {
  PinIrq* p = (PinIrq*)arg;
  p->count = p->count + 1;
  int s = p->slot;
  if (s >= 0) postEvent(s);
}

// Temporizadores del programa (timerBegin...)
static const int MAX_UTIMERS = 4;
struct UserTimer {
  hw_timer_t* t;
  volatile int8_t slot;
  volatile bool armed;
  bool alarmOn;
  bool reload;
  uint64_t alarm;
};
static UserTimer utm[MAX_UTIMERS];

static void ARDUINO_ISR_ATTR userTimerIsr(void* arg) {
  UserTimer* u = (UserTimer*)arg;
  int s = u->slot;
  if (u->armed && s >= 0) postEvent(s);
}

// Generador de ondas (senoidal, triangular, diente de sierra) por DDS:
// un temporizador saca 40000 muestras por segundo por el DAC (ESP32: pines
// 25 y 26) o por el modulador sigma-delta en cualquier otro pin.
static const uint32_t DDS_RATE = 40000;
static const int DDS_MAX = 4;
enum { DDS_OFF = 0, DDS_DAC = 1, DDS_SDM = 2 };
struct DdsChan {
  volatile uint8_t kind;
  uint8_t dacCh;
  int8_t pin;
  void* sdm;
  volatile uint32_t phase;
  volatile uint32_t step;
  uint8_t table[256];
};
static DdsChan dds[DDS_MAX];
static hw_timer_t* ddsTimer = nullptr;

static void ARDUINO_ISR_ATTR ddsIsr() {
  for (int c = 0; c < DDS_MAX; c++) {
    DdsChan& d = dds[c];
    uint8_t k = d.kind;
    if (k == DDS_OFF) continue;
    uint32_t ph = d.phase + d.step;
    d.phase = ph;
    uint8_t v = d.table[ph >> 24];
#if XRT_HAS_DAC
    if (k == DDS_DAC) {
      dac_ll_update_output_value((dac_channel_t)d.dacCh, v);
      continue;
    }
#endif
#if SOC_SDM_SUPPORTED
    if (k == DDS_SDM) {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0)
      sdm_channel_set_pulse_density((sdm_channel_handle_t)d.sdm, (int8_t)((int)v - 128));
#else
      sdm_channel_set_duty((sdm_channel_handle_t)d.sdm, (int8_t)((int)v - 128));
#endif
    }
#endif
  }
}

// Reproductor de secuencias (patternStart): un temporizador cambia varios pines
// a la vez, con una sola escritura en el registro de salida del GPIO.
struct PatMask { uint32_t set0, set1; };
struct PatState {
  volatile bool on;
  uint8_t* seq;
  PatMask* lut;
  int len;
  volatile int pos;
  uint32_t mask0, mask1;   // pines de la secuencia (0-31 y 32-63)
};
static PatState pat = {false, nullptr, nullptr, 0, 0, 0, 0};
static hw_timer_t* patTimer = nullptr;
static uint32_t patRes = 0;

static void ARDUINO_ISR_ATTR patIsr() {
  if (!pat.on) return;
  int p = pat.pos;
  const PatMask m = pat.lut[pat.seq[p]];
  if (++p >= pat.len) p = 0;
  pat.pos = p;
  REG_WRITE(GPIO_OUT_REG, (REG_READ(GPIO_OUT_REG) & ~pat.mask0) | m.set0);
#ifdef GPIO_OUT1_REG
  if (pat.mask1) REG_WRITE(GPIO_OUT1_REG, (REG_READ(GPIO_OUT1_REG) & ~pat.mask1) | m.set1);
#endif
}

static int dacChannelOf(int pin) {
#if CONFIG_IDF_TARGET_ESP32
  if (pin == 25) return 0;
  if (pin == 26) return 1;
#elif CONFIG_IDF_TARGET_ESP32S2
  if (pin == 17) return 0;
  if (pin == 18) return 1;
#endif
  (void)pin;
  return -1;
}

static ledc_channel_handle_t* ledcBus(int pin) {
  if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return nullptr;
  return (ledc_channel_handle_t*)perimanGetPinBus((uint8_t)pin, ESP32_BUS_TYPE_LEDC);
}

static TwoWire* wireBus(int b) {
#if XRT_HAS_WIRE1
  if (b == 1) return &Wire1;
#endif
  return b == 0 ? &Wire : nullptr;
}

// ---------------------------------------------------------------------
//  Hardware para el interprete
// ---------------------------------------------------------------------
class EspHal : public minic::Hal {
 public:
  uint64_t outPins = 0, pwmPins = 0, tonePins = 0, rgbPins = 0;
  bool rgbUsed = false;
  uint32_t lastYield = 0;
  char perr[120];
  char nerr[160];

  // estado de las librerias
  struct ChCfg { uint32_t freq; uint8_t res; bool setup; uint64_t pins; int64_t duty; };
  ChCfg chCfg[XRT_LEDC_CH];
  bool chMode = false;                 // el programa usa la API 2.x (ledcSetup/canales)
  uint32_t awFreq = 1000;
  uint8_t awRes = 8;
  uint32_t pinAwFreq[64];
  uint8_t pinAwRes[64];
  struct ServoCh { int8_t pin; uint16_t minUs, maxUs, us; };
  ServoCh servos[16];
  uint32_t slotUsed = 0;
  uint32_t idfCh[2] = {0, 0};          // canales LEDC configurados con la API de ESP-IDF
  bool idfFade = false;
  bool wireUsed[2] = {false, false};
  bool spiUsed = false;
  uint32_t bootCpuMhz = 0;

  EspHal() {
    evBits = &eventBits;
    clearTables();
  }

  void clearTables() {
    for (int c = 0; c < XRT_LEDC_CH; c++) { chCfg[c] = ChCfg{0, 0, false, 0, -1}; }
    chMode = false;
    awFreq = 1000;
    awRes = 8;
    memset(pinAwFreq, 0, sizeof pinAwFreq);
    memset(pinAwRes, 0, sizeof pinAwRes);
    for (int s = 0; s < 16; s++) servos[s] = ServoCh{-1, 544, 2400, 1500};
  }

  static bool isRgb(int pin) {
#ifdef RGB_BUILTIN
    return pin == RGB_BUILTIN;
#else
    (void)pin;
    return false;
#endif
  }

  const char* pinError(int pin, bool output) override {
    if (isRgb(pin)) return nullptr;
    bool valid = pin >= 0 && pin < SOC_GPIO_PIN_COUNT;
#ifdef GPIO_IS_VALID_GPIO
    if (valid) valid = GPIO_IS_VALID_GPIO(pin);
#endif
    if (!valid) { snprintf(perr, sizeof perr, "el pin %d no existe en este ESP32", pin); return perr; }
#if CONFIG_IDF_TARGET_ESP32
    if (pin >= 6 && pin <= 11) { snprintf(perr, sizeof perr, "el pin %d es de la memoria flash (los 6 a 11 no se pueden usar); prueba 25, 26, 27, 32 o 33", pin); return perr; }
    if (output && pin >= 34) { snprintf(perr, sizeof perr, "el pin %d solo puede usarse como entrada", pin); return perr; }
#elif CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32S2
    if (pin >= 26 && pin <= 32) { snprintf(perr, sizeof perr, "el pin %d esta reservado para la memoria flash/PSRAM", pin); return perr; }
#elif CONFIG_IDF_TARGET_ESP32C3
    if (pin >= 12 && pin <= 17) { snprintf(perr, sizeof perr, "el pin %d esta reservado para la memoria flash", pin); return perr; }
#endif
    (void)output;
    return nullptr;
  }

  void markOut(int pin) {
    if (isRgb(pin)) rgbUsed = true;
    else if (pin >= 0 && pin < 64) outPins |= (1ULL << pin);
  }

  void pinMode(int pin, int mode) override {
    uint8_t m = INPUT;
    switch (mode) {
      case minic::MC_OUTPUT: m = OUTPUT; markOut(pin); break;
      case minic::MC_OUTPUT_OPEN_DRAIN: m = OUTPUT_OPEN_DRAIN; markOut(pin); break;
      case minic::MC_INPUT_PULLUP: m = INPUT_PULLUP; break;
      case minic::MC_INPUT_PULLDOWN: m = INPUT_PULLDOWN; break;
      default: m = INPUT; break;
    }
    ::pinMode(pin, m);
  }
  void digitalWrite(int pin, int v) override { markOut(pin); ::digitalWrite(pin, v ? HIGH : LOW); }
  int digitalRead(int pin) override { return ::digitalRead(pin); }
  int analogRead(int pin) override { return ::analogRead(pin); }
  void analogWrite(int pin, int v) override {
    if (v < 0) v = 0;
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT || pin >= 64) { ::analogWrite(pin, v); return; }
    pwmPins |= (1ULL << pin);
    outPins |= (1ULL << pin);
    ledc_channel_handle_t* b = ledcBus(pin);
    uint8_t res;
    if (b) {
      res = b->channel_resolution;
    } else {
      res = pinAwRes[pin] ? pinAwRes[pin] : awRes;
      ::analogWriteResolution(pin, res);   // pin sin PWM: fija la resolucion y la frecuencia del siguiente
      ::analogWriteFrequency(pin, pinAwFreq[pin] ? pinAwFreq[pin] : awFreq);
    }
    int maxv = (1 << res) - 1;
    if (v > maxv) v = maxv;
    ::analogWrite(pin, v);
  }
  void analogReadResolution(int bits) override { ::analogReadResolution(bits); }
  void dacWrite(int pin, int v) override {
#if XRT_HAS_DAC
    stopDds(pin);
    markOut(pin);
    ::dacWrite(pin, (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)));
#else
    (void)pin; (void)v;
    write("[X-RTbox] dacWrite no existe en este ESP32 (no tiene DAC)\r\n", 58);
#endif
  }
  int touchRead(int pin) override {
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2 || CONFIG_IDF_TARGET_ESP32S3
    return (int)::touchRead(pin);
#else
    (void)pin;
    return 0;
#endif
  }
  void tone(int pin, unsigned freq, unsigned long dur) override {
    if (pin >= 0 && pin < 64) { tonePins |= (1ULL << pin); outPins |= (1ULL << pin); }
    ::tone(pin, freq, dur);
  }
  void noTone(int pin) override { ::noTone(pin); }
  unsigned long pulseIn(int pin, int state, unsigned long timeoutUs) override {
    return ::pulseIn(pin, state ? HIGH : LOW, timeoutUs);
  }
  uint32_t millis() override { return ::millis(); }
  uint32_t micros() override { return ::micros(); }
  void delayMs(uint32_t ms) override {
    // espera, pero se despierta en cuanto llega una interrupcion
    if (!eventBits && ms) {
      if (evSem) xSemaphoreTake(evSem, pdMS_TO_TICKS(ms));
      else ::delay(ms);
    }
    lastYield = ::millis();
  }
  void delayUs(uint32_t us) override { ::delayMicroseconds(us); }
  void write(const char* s, size_t n) override {
    if (Serial) Serial.write((const uint8_t*)s, n);
    if (bleConnected.load()) consoleOut.write(s, n);
  }
  // La tarea de comunicacion pasa aqui lo que llega por USB o Bluetooth
  int available() override { return (int)progInput.size(); }
  int read() override {
    char c;
    if (progInput.read(&c, 1) == 1) return (unsigned char)c;
    return -1;
  }
  int peek() override { return progInput.peek(); }
  void yield() override {
    // deja correr al resto del sistema de vez en cuando (watchdog)
    uint32_t now = ::millis();
    if (now - lastYield >= 200) {
      vTaskDelay(1);
      lastYield = ::millis();
    }
  }
  uint32_t entropy() override { return esp_random(); }
  int ledBuiltin() override {
#ifdef LED_BUILTIN
    return LED_BUILTIN;
#else
    return 2;
#endif
  }

  int pinConstant(const std::string& n) override {
    if (n == "SDA") return SDA;
    if (n == "SCL") return SCL;
    if (n == "MOSI") return MOSI;
    if (n == "MISO") return MISO;
    if (n == "SCK") return SCK;
    if (n == "SS") return SS;
    if (n == "TX") return TX;
    if (n == "RX") return RX;
    // ESP-IDF: valores que cambian segun el chip
    if (n == "LEDC_LOW_SPEED_MODE") return LEDC_LOW_SPEED_MODE;
#if SOC_LEDC_SUPPORT_HS_MODE
    if (n == "LEDC_HIGH_SPEED_MODE") return LEDC_HIGH_SPEED_MODE;
#else
    if (n == "LEDC_HIGH_SPEED_MODE") return LEDC_LOW_SPEED_MODE;   // este chip solo tiene el modo de baja velocidad
#endif
    if (n == "LEDC_SPEED_MODE_MAX") return LEDC_SPEED_MODE_MAX;
    if (n == "LEDC_CHANNEL_MAX") return LEDC_CHANNEL_MAX;
    if (n == "LEDC_TIMER_MAX") return LEDC_TIMER_MAX;
    if (n == "LEDC_TIMER_BIT_MAX") return LEDC_TIMER_BIT_MAX;
    if (n == "LEDC_AUTO_CLK") return (int)LEDC_AUTO_CLK;
    if (n == "LEDC_USE_RC_FAST_CLK" || n == "LEDC_USE_RTC8M_CLK") return (int)LEDC_USE_RC_FAST_CLK;
#if SOC_LEDC_SUPPORT_APB_CLOCK
    if (n == "LEDC_USE_APB_CLK") return (int)LEDC_USE_APB_CLK;
#endif
#if SOC_LEDC_SUPPORT_REF_TICK
    if (n == "LEDC_USE_REF_TICK") return (int)LEDC_USE_REF_TICK;
#endif
#if SOC_LEDC_SUPPORT_XTAL_CLOCK
    if (n == "LEDC_USE_XTAL_CLK") return (int)LEDC_USE_XTAL_CLK;
#endif
#if SOC_LEDC_SUPPORT_PLL_DIV_CLOCK
    if (n == "LEDC_USE_PLL_DIV_CLK") return (int)LEDC_USE_PLL_DIV_CLK;
#endif
#if CONFIG_IDF_TARGET_ESP32
    if (n == "DAC1") return 25;
    if (n == "DAC2") return 26;
#elif CONFIG_IDF_TARGET_ESP32S2
    if (n == "DAC1") return 17;
    if (n == "DAC2") return 18;
#endif
    if (n.size() >= 2 && n.size() <= 3 && n[0] == 'A' && isdigit((unsigned char)n[1]) &&
        (n.size() == 2 || isdigit((unsigned char)n[2]))) {
      int ch = atoi(n.c_str() + 1);
      int p = analogChannelToDigitalPin((uint8_t)ch);
      if (p >= 0) return p;
    }
    return -1;
  }

  // ---- eventos (interrupciones) ----
  uint32_t takeEvents() override {
    portENTER_CRITICAL(&evMux);
    uint32_t b = eventBits;
    eventBits = 0;
    portEXIT_CRITICAL(&evMux);
    return b;
  }
  uint32_t takeEventCount(int s) override {
    if (s < 0 || s >= 32) return 0;
    portENTER_CRITICAL(&evMux);
    uint32_t c = evCount[s];
    evCount[s] = 0;
    portEXIT_CRITICAL(&evMux);
    return c;
  }
  int allocSlot() {
    for (int s = 0; s < 32; s++) {
      if (slotUsed & (1u << s)) continue;
      slotUsed |= 1u << s;
      portENTER_CRITICAL(&evMux);
      evCount[s] = 0;
      eventBits = eventBits & ~(1u << s);
      portEXIT_CRITICAL(&evMux);
      return s;
    }
    return -1;
  }
  void freeSlot(int s) {
    if (s < 0 || s >= 32) return;
    slotUsed &= ~(1u << s);
    portENTER_CRITICAL(&evMux);
    evCount[s] = 0;
    eventBits = eventBits & ~(1u << s);
    portEXIT_CRITICAL(&evMux);
  }

  std::string nativeStr(int fn) override {
    if (fn == minic::NS_CHIP_MODEL) return std::string(ESP.getChipModel());
    if (fn == minic::NS_SDK_VERSION) return std::string(ESP.getSdkVersion());
    return std::string();
  }

  double nfail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(nerr, sizeof nerr, fmt, ap);
    va_end(ap);
    nativeError = nerr;
    return 0;
  }
  bool pinOk(int pin, bool out) {
    const char* e = pinError(pin, out);
    if (e) { nativeError = e; return false; }
    if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) { nativeError = "este pin no sirve para esto"; return false; }
    return true;
  }
  void unlinkChannels(int pin) {
    for (int c = 0; c < XRT_LEDC_CH; c++) chCfg[c].pins &= ~(1ULL << pin);
  }
  void releaseLedc(int pin) {
    if (ledcBus(pin)) ::ledcDetach((uint8_t)pin);
    unlinkChannels(pin);
  }
  bool badRes(uint32_t freq, int res) {
    if (freq < 1) { nfail("la frecuencia debe ser mayor que 0"); return true; }
    if (res < 1 || res > SOC_LEDC_TIMER_BIT_WIDTH) { nfail("la resolucion debe estar entre 1 y %d bits", SOC_LEDC_TIMER_BIT_WIDTH); return true; }
    if ((double)freq * (double)(1UL << res) > 80000000.0) {
      nfail("con %d bits la frecuencia maxima es %lu Hz (baja la resolucion)", res, (unsigned long)(80000000UL >> res));
      return true;
    }
    return false;
  }
  // En la API 2.x el primer argumento es un canal; en la 3.x es un pin.
  int ledcTarget(int x) {
    if (chMode && x >= 0 && x < XRT_LEDC_CH && chCfg[x].setup) {
      if (!chCfg[x].pins) return -2;
      return __builtin_ctzll(chCfg[x].pins);
    }
    return x;
  }
  int ledcTargetOrFail(int x) {
    int p = ledcTarget(x);
    if (p == -2) { nfail("el canal %d no tiene ningun pin: usa ledcAttachPin(pin, %d)", x, x); return -1; }
    return p;
  }

  // ---- generadores ----
  int ddsFind(int pin) {
    for (int c = 0; c < DDS_MAX; c++)
      if (dds[c].kind != DDS_OFF && dds[c].pin == pin) return c;
    return -1;
  }
  void stopDds(int pin) {
    int c = ddsFind(pin);
    if (c < 0) return;
    dds[c].kind = DDS_OFF;
    dds[c].pin = -1;
    ::pinMode(pin, OUTPUT);     // libera el DAC / sigma-delta del pin
    ::digitalWrite(pin, LOW);
    bool any = false;
    for (int k = 0; k < DDS_MAX; k++) any = any || dds[k].kind != DDS_OFF;
    if (!any && ddsTimer) {
      ::timerEnd(ddsTimer);
      ddsTimer = nullptr;
    }
  }
  double startWave(int fn, const double* a, int n) {
    int pin = (int)a[0];
    double f = a[1];
    double amp = n > 2 ? a[2] : 1.65;
    double off = n > 3 ? a[3] : 1.65;
    if (!(f > 0) || f > DDS_RATE / 4) return nfail("la frecuencia debe estar entre 0 y %lu Hz (para mas usa signalSquare)", (unsigned long)(DDS_RATE / 4));
    if (amp < 0) return nfail("la amplitud no puede ser negativa");
    uint8_t tab[256];
    for (int i = 0; i < 256; i++) {
      double w;
      if (fn == minic::NF_SIG_SINE) w = sin(2.0 * M_PI * i / 256.0);
      else if (fn == minic::NF_SIG_TRIANGLE) w = i < 128 ? -1.0 + i / 64.0 : 3.0 - i / 64.0;
      else w = -1.0 + i / 127.5;
      double v = (off + amp * w) * 255.0 / 3.3;
      tab[i] = (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : lround(v)));
    }
    int c = ddsFind(pin);
    if (c < 0) {
      for (int k = 0; k < DDS_MAX && c < 0; k++)
        if (dds[k].kind == DDS_OFF) c = k;
      if (c < 0) return nfail("como maximo %d senales a la vez", DDS_MAX);
    }
    uint8_t kind = dds[c].kind;
    dds[c].kind = DDS_OFF;
    if (kind == DDS_OFF) {
      releaseLedc(pin);
      int dch = dacChannelOf(pin);
      if (dch >= 0) {
#if XRT_HAS_DAC
        if (!::dacWrite((uint8_t)pin, tab[0])) return nfail("no se pudo activar el DAC del pin %d", pin);
        dds[c].dacCh = (uint8_t)dch;
        kind = DDS_DAC;
#endif
      } else {
#if SOC_SDM_SUPPORTED
        bool ok = ::sigmaDeltaAttach((uint8_t)pin, 10000000) || ::sigmaDeltaAttach((uint8_t)pin, 1000000) ||
                  ::sigmaDeltaAttach((uint8_t)pin, 312500);
        void* h = ok ? perimanGetPinBus((uint8_t)pin, ESP32_BUS_TYPE_SIGMADELTA) : nullptr;
        if (!h) return nfail("no se pudo usar el modulador sigma-delta en el pin %d (no quedan canales)", pin);
        dds[c].sdm = h;
        kind = DDS_SDM;
#else
        return nfail("el pin %d no puede sacar esta senal en este ESP32", pin);
#endif
      }
    }
    memcpy(dds[c].table, tab, sizeof tab);
    uint32_t step = (uint32_t)llround(f * 4294967296.0 / (double)DDS_RATE);
    dds[c].step = step;
    dds[c].pin = (int8_t)pin;
    markOut(pin);
    if (!ddsTimer) {
      ddsTimer = ::timerBegin(1000000);
      if (!ddsTimer) {
        ::pinMode(pin, OUTPUT);
        return nfail("no queda ningun temporizador libre para el generador");
      }
      ::timerAttachInterrupt(ddsTimer, ddsIsr);
      ::timerAlarm(ddsTimer, 1000000 / DDS_RATE, true, 0);
    }
    dds[c].kind = kind;
    return (double)step * (double)DDS_RATE / 4294967296.0;
  }
  double startSquare(const double* a, int n) {
    int pin = (int)a[0];
    double f = a[1];
    double duty = n > 2 ? a[2] : 50.0;
    if (!(f >= 1) || f > 40000000.0) return nfail("la frecuencia debe estar entre 1 Hz y 40 MHz");
    if (duty < 0 || duty > 100) return nfail("el ciclo de trabajo va de 0 a 100 (%%)");
    stopDds(pin);
    releaseLedc(pin);
    uint32_t fi = (uint32_t)lround(f);
    int res = XRT_LEDC_BITS;
    while (res > 1 && (double)fi * (double)(1UL << res) > 80000000.0) res--;
    bool ok = false;
    for (; res >= 1 && !ok; ) {
      ok = ::ledcAttach((uint8_t)pin, fi, (uint8_t)res);
      if (!ok) res--;
    }
    if (!ok) return nfail("no se pudo generar %lu Hz en el pin %d (no quedan temporizadores PWM libres)", (unsigned long)fi, pin);
    markOut(pin);
    uint32_t full = 1UL << res;
    uint32_t d = (uint32_t)lround(duty / 100.0 * full);
    if (d >= full) d = full;
    ::ledcWrite((uint8_t)pin, d);
    return (double)::ledcReadFreq((uint8_t)pin);
  }

  // ---- ESP-IDF ----
  bool idfMode(int m) {
    if (m >= 0 && m < LEDC_SPEED_MODE_MAX) return true;
#if SOC_LEDC_SUPPORT_HS_MODE
    nfail("speed_mode no valido: usa LEDC_LOW_SPEED_MODE o LEDC_HIGH_SPEED_MODE");
#else
    nfail("speed_mode no valido: usa LEDC_LOW_SPEED_MODE");
#endif
    return false;
  }
  bool idfChan(int c) {
    if (c >= 0 && c < LEDC_CHANNEL_MAX) return true;
    nfail("canal no valido (de LEDC_CHANNEL_0 a LEDC_CHANNEL_%d)", LEDC_CHANNEL_MAX - 1);
    return false;
  }
  bool idfTimer(int t) {
    if (t >= 0 && t < LEDC_TIMER_MAX) return true;
    nfail("temporizador no valido (de LEDC_TIMER_0 a LEDC_TIMER_%d)", LEDC_TIMER_MAX - 1);
    return false;
  }
  void idfEnsureFade() {
    if (idfFade) return;
    if (ledc_fade_func_install(0) == ESP_OK) idfFade = true;
  }
  void idfTakePin(int gpio, bool out) {   // el pin pasa a la API de ESP-IDF
    stopDds(gpio);
    releaseLedc(gpio);
    if (out) markOut(gpio);
  }

  // ---- registros del GPIO ----
  uint32_t gpioSafeMask(int bank) {   // pines que el programa puede tocar (no los de la memoria flash)
    uint32_t m = 0;
    for (int k = 0; k < 32; k++) {
      int p = bank * 32 + k;
      if (p < SOC_GPIO_PIN_COUNT && pinError(p, true) == nullptr) m |= 1u << k;
    }
    return m;
  }
  bool gpioReg(int r, uint32_t& addr) {
    switch (r) {
      case 0: addr = GPIO_OUT_REG; return true;
      case 1: addr = GPIO_OUT_W1TS_REG; return true;
      case 2: addr = GPIO_OUT_W1TC_REG; return true;
      case 3: addr = GPIO_ENABLE_REG; return true;
      case 4: addr = GPIO_ENABLE_W1TS_REG; return true;
      case 5: addr = GPIO_ENABLE_W1TC_REG; return true;
      case 6: addr = GPIO_IN_REG; return true;
#ifdef GPIO_OUT1_REG
      case 7: addr = GPIO_OUT1_REG; return true;
      case 8: addr = GPIO_OUT1_W1TS_REG; return true;
      case 9: addr = GPIO_OUT1_W1TC_REG; return true;
      case 10: addr = GPIO_ENABLE1_REG; return true;
      case 11: addr = GPIO_ENABLE1_W1TS_REG; return true;
      case 12: addr = GPIO_ENABLE1_W1TC_REG; return true;
      case 13: addr = GPIO_IN1_REG; return true;
#endif
      default: break;
    }
    nfail(r >= 7 && r <= 13 ? "este ESP32 no tiene pines del 32 en adelante (usa los registros sin '1')" : "registro GPIO no valido");
    return false;
  }
  double gpioRegWrite(int r, uint32_t v) {
    uint32_t addr;
    if (!gpioReg(r, addr)) return 0;
    int bank = r >= 7 ? 1 : 0;
    uint32_t safe = gpioSafeMask(bank);
    int kind = (r >= 7 ? r - 7 : r);   // 0 out, 1 w1ts, 2 w1tc, 3 enable, 4 enable_w1ts, 5 enable_w1tc, 6 in
    if (kind == 6) return nfail("GPIO.in solo se puede leer");
    if (kind == 3 || kind == 4) {   // habilitar salidas: nunca en los pines de la memoria flash
      uint32_t bad = v & ~safe;
      if (kind == 3) bad &= ~REG_READ(addr);
      if (bad) return nfail("ese valor activa como salida el pin %d, que no se puede usar", bank * 32 + __builtin_ctz(bad));
      for (int k = 0; k < 32; k++)
        if (v & (1u << k)) markOut(bank * 32 + k);
    }
    if (kind == 0 || kind == 3) REG_WRITE(addr, (REG_READ(addr) & ~safe) | (v & safe));
    else REG_WRITE(addr, v & safe);
    return 0;
  }

  // ---- reproductor de secuencias ----
  void stopPattern() {
    pat.on = false;
    if (patTimer) { ::timerEnd(patTimer); patTimer = nullptr; }
    if (pat.mask0) REG_WRITE(GPIO_OUT_W1TC_REG, pat.mask0);   // los pines quedan a 0
#ifdef GPIO_OUT1_REG
    if (pat.mask1) REG_WRITE(GPIO_OUT1_W1TC_REG, pat.mask1);
#endif
    free(pat.seq);
    free(pat.lut);
    pat.seq = nullptr;
    pat.lut = nullptr;
    pat.mask0 = pat.mask1 = 0;
    pat.len = 0;
  }
  uint64_t patAlarm(double hz) {
    double t = (double)patRes / hz;
    if (t < 1) t = 1;
    return (uint64_t)llround(t);
  }
  double startPattern(const int* pins, int np, const uint8_t* seq, int len, double hz) override {
    if (!(hz > 0) || hz > 100000) return nfail("pasosPorSegundo debe estar entre 0 y 100000");
    stopPattern();
    uint32_t m0 = 0, m1 = 0;
    for (int k = 0; k < np; k++) {
      int p = pins[k];
      if (!pinOk(p, true)) return 0;
      if (p >= 64) return nfail("el pin %d no sirve para la secuencia", p);
      for (int q = 0; q < k; q++)
        if (pins[q] == p) return nfail("el pin %d esta repetido", p);
      stopDds(p);
      releaseLedc(p);
      ::pinMode(p, OUTPUT);
      ::digitalWrite(p, LOW);
      markOut(p);
      if (p < 32) m0 |= 1u << p;
      else m1 |= 1u << (p - 32);
    }
    pat.lut = (PatMask*)malloc(sizeof(PatMask) << np);
    pat.seq = (uint8_t*)malloc((size_t)len);
    if (!pat.lut || !pat.seq) { stopPattern(); return nfail("no hay memoria para la secuencia"); }
    for (int v = 0; v < (1 << np); v++) {
      PatMask m = {0, 0};
      for (int k = 0; k < np; k++) {
        if (!(v & (1 << k))) continue;
        if (pins[k] < 32) m.set0 |= 1u << pins[k];
        else m.set1 |= 1u << (pins[k] - 32);
      }
      pat.lut[v] = m;
    }
    memcpy(pat.seq, seq, (size_t)len);
    pat.len = len;
    pat.pos = 0;
    pat.mask0 = m0;
    pat.mask1 = m1;
    static const uint32_t resList[] = {40000000, 10000000, 1000000};
    for (int k = 0; k < 3 && !patTimer; k++) {
      patTimer = ::timerBegin(resList[k]);
      if (patTimer) patRes = resList[k];
    }
    if (!patTimer) { stopPattern(); return nfail("no queda ningun temporizador libre"); }
    uint64_t alarm = patAlarm(hz);
    ::timerAttachInterrupt(patTimer, patIsr);
    pat.on = true;
    ::timerAlarm(patTimer, alarm, true, 0);
    return (double)patRes / (double)alarm;
  }

  UserTimer* timerOf(double h) {
    int i = (int)h - 1;
    if (i < 0 || i >= MAX_UTIMERS || !utm[i].t) { nfail("temporizador no valido: usa el valor que devuelve timerBegin()"); return nullptr; }
    return &utm[i];
  }
  ServoCh* servoOf(double h) {
    int i = (int)h - 1;
    if (i < 0 || i >= 16 || servos[i].pin < 0) { nfail("el servo no esta conectado: usa primero .attach(pin)"); return nullptr; }
    return &servos[i];
  }
  void servoWriteUs(ServoCh& s, double us) {
    if (us < s.minUs) us = s.minUs;
    if (us > s.maxUs) us = s.maxUs;
    s.us = (uint16_t)lround(us);
    uint32_t duty = (uint32_t)lround(us * (double)(1UL << XRT_LEDC_BITS) / 20000.0);
    ::ledcWrite((uint8_t)s.pin, duty);
  }

  double native(int fn, const double* a, int n) override {
    using namespace minic;
    switch (fn) {
      // ---------------- PWM (LEDC) ----------------
      case NF_LEDC_ATTACH: case NF_LEDC_ATTACH_CH: {
        int pin = (int)a[0];
        uint32_t f = (uint32_t)lround(a[1]);
        int res = (int)a[2];
        if (badRes(f, res)) return 0;
        stopDds(pin);
        releaseLedc(pin);
        markOut(pin);
        bool ok;
        if (fn == NF_LEDC_ATTACH_CH) {
          int ch = (int)a[3];
          if (ch < 0 || ch >= XRT_LEDC_CH) return nfail("el canal debe estar entre 0 y %d", XRT_LEDC_CH - 1);
          ok = ::ledcAttachChannel((uint8_t)pin, f, (uint8_t)res, (uint8_t)ch);
        } else {
          ok = ::ledcAttach((uint8_t)pin, f, (uint8_t)res);
        }
        if (!ok) return nfail("no se pudo poner %lu Hz con %d bits en el pin %d (no quedan temporizadores PWM: usa la misma frecuencia y resolucion que otro pin)", (unsigned long)f, res, pin);
        return 1;
      }
      case NF_LEDC_SETUP: {
        int ch = (int)a[0];
        uint32_t f = (uint32_t)lround(a[1]);
        int res = (int)a[2];
        if (ch < 0 || ch >= XRT_LEDC_CH) return nfail("el canal debe estar entre 0 y %d", XRT_LEDC_CH - 1);
        if (badRes(f, res)) return 0;
        chMode = true;
        chCfg[ch].freq = f;
        chCfg[ch].res = (uint8_t)res;
        chCfg[ch].setup = true;
        if (chCfg[ch].pins) ::ledcChangeFrequency((uint8_t)__builtin_ctzll(chCfg[ch].pins), f, (uint8_t)res);
        return f;
      }
      case NF_LEDC_ATTACH_PIN: {
        int pin = (int)a[0], ch = (int)a[1];
        if (ch < 0 || ch >= XRT_LEDC_CH || !chCfg[ch].setup) return nfail("usa antes ledcSetup(%d, frecuencia, bits)", ch);
        stopDds(pin);
        releaseLedc(pin);
        markOut(pin);
        if (!::ledcAttachChannel((uint8_t)pin, chCfg[ch].freq, chCfg[ch].res, (uint8_t)ch))
          return nfail("no se pudo conectar el pin %d al canal %d", pin, ch);
        chCfg[ch].pins |= 1ULL << pin;
        if (chCfg[ch].duty >= 0) ::ledcWrite((uint8_t)pin, (uint32_t)chCfg[ch].duty);
        return 0;
      }
      case NF_LEDC_DETACH: {
        int pin = (int)a[0];
        bool had = ledcBus(pin) != nullptr;
        releaseLedc(pin);
        return had;
      }
      case NF_LEDC_WRITE: {
        int x = (int)a[0];
        uint32_t duty = a[1] < 0 ? 0 : (uint32_t)a[1];
        if (chMode && x >= 0 && x < XRT_LEDC_CH && chCfg[x].setup) {
          chCfg[x].duty = duty;
          if (!chCfg[x].pins) return 1;   // se aplicara al conectar el pin
          return ::ledcWrite((uint8_t)__builtin_ctzll(chCfg[x].pins), duty);
        }
        if (!ledcBus(x)) {
          if (!pinOk(x, true)) return 0;
          return nfail("el pin %d no tiene PWM: usa antes ledcAttach(%d, frecuencia, bits)", x, x);
        }
        return ::ledcWrite((uint8_t)x, duty);
      }
      case NF_LEDC_TONE: case NF_LEDC_NOTE: {
        int p = ledcTargetOrFail((int)a[0]);
        if (p < 0) return 0;
        uint32_t f = 0;
        if (fn == NF_LEDC_TONE) {
          f = a[1] < 0 ? 0 : (uint32_t)lround(a[1]);
        } else {
          int note = (int)a[1], oct = (int)a[2];
          if (note < 0 || note > 11 || oct < 0 || oct > 8) return nfail("nota 0..11 (NOTE_C..NOTE_B) y octava 0..8");
          static const uint16_t base[12] = {4186, 4435, 4699, 4978, 5274, 5588, 5920, 6272, 6645, 7040, 7459, 7902};
          f = base[note] >> (8 - oct);
        }
        if (!ledcBus(p)) {
          if (!pinOk(p, true)) return 0;
          if (f == 0) return 0;
          stopDds(p);
          markOut(p);
          if (!::ledcAttach((uint8_t)p, f, 10)) return nfail("no se pudo sacar el tono por el pin %d", p);
        }
        if (fn == NF_LEDC_TONE) return ::ledcWriteTone((uint8_t)p, f);
        return ::ledcWriteNote((uint8_t)p, (note_t)(int)a[1], (uint8_t)a[2]);
      }
      case NF_LEDC_READ: case NF_LEDC_READ_FREQ: {
        int p = ledcTarget((int)a[0]);
        if (p < 0 || !ledcBus(p)) return 0;
        return fn == NF_LEDC_READ ? ::ledcRead((uint8_t)p) : ::ledcReadFreq((uint8_t)p);
      }
      case NF_LEDC_CHANGE_FREQ: {
        int x = (int)a[0];
        int p = ledcTargetOrFail(x);
        if (p < 0) return 0;
        uint32_t f = (uint32_t)lround(a[1]);
        int res = (int)a[2];
        if (badRes(f, res)) return 0;
        if (!ledcBus(p)) return nfail("el pin %d no tiene PWM: usa antes ledcAttach()", p);
        uint32_t got = ::ledcChangeFrequency((uint8_t)p, f, (uint8_t)res);
        if (!got) return nfail("no se pudo cambiar a %lu Hz con %d bits", (unsigned long)f, res);
        if (chMode && x >= 0 && x < XRT_LEDC_CH && chCfg[x].setup) { chCfg[x].freq = f; chCfg[x].res = (uint8_t)res; }
        return got;
      }
      case NF_LEDC_FADE: {
        int pin = (int)a[0];
        if (!ledcBus(pin)) return nfail("el pin %d no tiene PWM: usa antes ledcAttach()", pin);
        return ::ledcFade((uint8_t)pin, (uint32_t)a[1], (uint32_t)a[2], (int)a[3]);
      }
      case NF_AW_FREQ: case NF_AW_RES: {
        bool perPin = n == 2;
        int pin = perPin ? (int)a[0] : -1;
        double v = perPin ? a[1] : a[0];
        if (perPin && (!pinOk(pin, true) || pin >= 64)) return 0;
        if (fn == NF_AW_FREQ) {
          if (!(v >= 1)) return nfail("la frecuencia debe ser mayor que 0");
          if (perPin) pinAwFreq[pin] = (uint32_t)lround(v);
          else awFreq = (uint32_t)lround(v);
        } else {
          if (v < 1 || v > SOC_LEDC_TIMER_BIT_WIDTH) return nfail("la resolucion debe estar entre 1 y %d bits", SOC_LEDC_TIMER_BIT_WIDTH);
          if (perPin) pinAwRes[pin] = (uint8_t)v;
          else awRes = (uint8_t)v;
        }
        // aplica el cambio a los pines que ya estan sacando analogWrite
        for (int p = 0; p < SOC_GPIO_PIN_COUNT && p < 64; p++) {
          if (perPin ? p != pin : (!(pwmPins & (1ULL << p)))) continue;
          ledc_channel_handle_t* b = ledcBus(p);
          if (!b) continue;
          uint32_t f = fn == NF_AW_FREQ ? (uint32_t)lround(v) : b->freq_hz;
          uint8_t r = fn == NF_AW_RES ? (uint8_t)v : b->channel_resolution;
          if (!perPin && ((fn == NF_AW_FREQ && pinAwFreq[p]) || (fn == NF_AW_RES && pinAwRes[p]))) continue;
          if (!::ledcChangeFrequency((uint8_t)p, f, r)) return nfail("no se puede poner %lu Hz con %d bits", (unsigned long)f, r);
        }
        return 0;
      }

      // ---------------- generadores de senal ----------------
      case NF_SIG_SINE: case NF_SIG_TRIANGLE: case NF_SIG_SAW:
        return startWave(fn, a, n);
      case NF_SIG_SQUARE:
        return startSquare(a, n);
      case NF_SIG_STOP: {
        int pin = (int)a[0];
        if (ddsFind(pin) >= 0) { stopDds(pin); return 0; }
        if (ledcBus(pin)) {
          releaseLedc(pin);
          ::pinMode(pin, OUTPUT);
          ::digitalWrite(pin, LOW);
        }
        return 0;
      }
      case NF_DAC_DISABLE: {
        int pin = (int)a[0];
#if XRT_HAS_DAC
        if (ddsFind(pin) >= 0) stopDds(pin);
        ::dacDisable((uint8_t)pin);
        return 0;
#else
        (void)pin;
        return nfail("este ESP32 no tiene DAC");
#endif
      }

      // ---------------- contadores y medida ----------------
      case NF_CNT_START: {
        int pin = (int)a[0];
        int mode = n > 1 ? (int)a[1] : RISING;
        if (mode < 1 || mode > 3) return nfail("el modo debe ser RISING, FALLING o CHANGE");
        PinIrq& q = pinIrq[pin];
        if (q.active) { q.counter = true; return 0; }
        q.count = 0;
        q.slot = -1;
        q.counter = true;
        q.active = true;
        ::attachInterruptArg((uint8_t)pin, pinIsr, &q, mode);
        return 1;
      }
      case NF_CNT_READ: {
        int pin = (int)a[0];
        return pinIrq[pin].active ? (double)pinIrq[pin].count : 0.0;
      }
      case NF_CNT_RESET:
        pinIrq[(int)a[0]].count = 0;
        return 0;
      case NF_CNT_STOP: {
        int pin = (int)a[0];
        PinIrq& q = pinIrq[pin];
        q.counter = false;
        if (q.active && q.slot < 0) {
          ::detachInterrupt((uint8_t)pin);
          q.active = false;
        }
        return 0;
      }
      case NF_PULSEIN_LONG: {
        unsigned long to = n > 2 ? (unsigned long)a[2] : 1000000UL;
        return (double)::pulseInLong((uint8_t)a[0], a[1] != 0 ? HIGH : LOW, to);
      }
      case NF_PIN_EVENT_ATTACH: {
        int pin = (int)a[0];
        int mode = (int)a[1];
        if (!((mode >= 1 && mode <= 5) || mode == 0x0C || mode == 0x0D))
          return nfail("el modo debe ser RISING, FALLING, CHANGE, ONLOW u ONHIGH");
        PinIrq& q = pinIrq[pin];
        int s = q.slot;
        if (s < 0) {
          s = allocSlot();
          if (s < 0) return nfail("demasiadas interrupciones a la vez (maximo 32)");
        }
        if (!q.active) q.count = 0;
        q.slot = (int8_t)s;
        q.active = true;
        ::attachInterruptArg((uint8_t)pin, pinIsr, &q, mode);
        return s;
      }
      case NF_PIN_EVENT_DETACH: {
        int pin = (int)a[0];
        if (pin < 0 || pin >= SOC_GPIO_PIN_COUNT) return -1;
        PinIrq& q = pinIrq[pin];
        int s = q.slot;
        if (!q.active || s < 0) return -1;
        q.slot = -1;
        freeSlot(s);
        if (!q.counter) {
          ::detachInterrupt((uint8_t)pin);
          q.active = false;
        }
        return s;
      }

      // ---------------- temporizadores ----------------
      case NF_TIMER_BEGIN: {
        double f = a[0];
        if (!(f >= 1) || f > 80000000.0) return nfail("frecuencia no valida (lo normal es timerBegin(1000000) = 1 MHz)");
        int i = -1;
        for (int k = 0; k < MAX_UTIMERS && i < 0; k++)
          if (!utm[k].t) i = k;
        if (i < 0) return nfail("como maximo %d temporizadores a la vez", MAX_UTIMERS);
        hw_timer_t* t = ::timerBegin((uint32_t)lround(f));
        if (!t) return nfail("no se pudo crear el temporizador a %.0f Hz (usa entre 1250 Hz y 40 MHz, o no quedan libres)", f);
        utm[i].t = t;
        utm[i].slot = -1;
        utm[i].armed = true;
        utm[i].alarmOn = false;
        utm[i].reload = true;
        utm[i].alarm = 0;
        return i + 1;
      }
      case NF_TIMER_ATTACH: {
        UserTimer* u = timerOf(a[0]);
        if (!u) return 0;
        int s = u->slot;
        if (s < 0) {
          s = allocSlot();
          if (s < 0) return nfail("demasiadas interrupciones a la vez (maximo 32)");
        }
        u->slot = (int8_t)s;
        ::timerAttachInterruptArg(u->t, userTimerIsr, u);
        return s;
      }
      case NF_TIMER_DETACH: {
        UserTimer* u = timerOf(a[0]);
        if (!u) return -1;
        ::timerDetachInterrupt(u->t);
        int s = u->slot;
        u->slot = -1;
        freeSlot(s);
        return s;
      }
      case NF_TIMER_ALARM: {
        UserTimer* u = timerOf(a[0]);
        if (!u) return 0;
        u->alarm = (uint64_t)a[1];
        u->reload = n > 2 ? a[2] != 0 : true;
        u->armed = true;
        u->alarmOn = true;
        ::timerAlarm(u->t, u->alarm, u->reload, n > 3 ? (uint64_t)a[3] : 0);
        return 0;
      }
      case NF_TIMER_ALARM_WRITE: {
        UserTimer* u = timerOf(a[0]);
        if (!u) return 0;
        u->alarm = (uint64_t)a[1];
        u->reload = n > 2 ? a[2] != 0 : true;
        if (u->alarmOn) ::timerAlarm(u->t, u->alarm, u->reload, 0);
        return 0;
      }
      case NF_TIMER_ALARM_ENABLE: {
        UserTimer* u = timerOf(a[0]);
        if (!u) return 0;
        if (!u->alarm) return nfail("usa antes timerAlarmWrite(timer, valor, true)");
        u->armed = true;
        u->alarmOn = true;
        ::timerAlarm(u->t, u->alarm, u->reload, 0);
        return 0;
      }
      case NF_TIMER_ALARM_DISABLE: {
        UserTimer* u = timerOf(a[0]);
        if (!u) return 0;
        u->armed = false;
        u->alarmOn = false;
        return 0;
      }
      case NF_TIMER_START: case NF_TIMER_STOP: case NF_TIMER_RESTART: case NF_TIMER_END:
      case NF_TIMER_READ: case NF_TIMER_READ_US: case NF_TIMER_READ_MS: case NF_TIMER_READ_S:
      case NF_TIMER_WRITE: {
        UserTimer* u = timerOf(a[0]);
        if (!u) return 0;
        switch (fn) {
          case NF_TIMER_START: ::timerStart(u->t); return 0;
          case NF_TIMER_STOP: ::timerStop(u->t); return 0;
          case NF_TIMER_RESTART: ::timerRestart(u->t); return 0;
          case NF_TIMER_READ: return (double)::timerRead(u->t);
          case NF_TIMER_READ_US: return (double)::timerReadMicros(u->t);
          case NF_TIMER_READ_MS: return (double)::timerReadMillis(u->t);
          case NF_TIMER_READ_S: return ::timerReadSeconds(u->t);
          case NF_TIMER_WRITE: ::timerWrite(u->t, (uint64_t)a[1]); return 0;
          default: break;
        }
        endTimer(*u);   // NF_TIMER_END
        return 0;
      }

      // ---------------- I2C ----------------
      case NF_WIRE_BEGIN: case NF_WIRE_END: case NF_WIRE_CLOCK: case NF_WIRE_GETCLOCK: case NF_WIRE_BEGINTX:
      case NF_WIRE_WRITE: case NF_WIRE_ENDTX: case NF_WIRE_REQUEST: case NF_WIRE_AVAILABLE: case NF_WIRE_READ:
      case NF_WIRE_PEEK: {
        int b = (int)a[0];
        TwoWire* w = wireBus(b);
        if (!w) return nfail("este ESP32 solo tiene un bus I2C: usa Wire");
        switch (fn) {
          case NF_WIRE_BEGIN: {
            int argc = n - 1;
            bool ok;
            if (argc == 0) ok = w->begin();
            else if (argc == 1) return nfail("el modo esclavo (Wire.begin(direccion)) no esta soportado: usa Wire.begin() o Wire.begin(SDA, SCL)");
            else {
              if (!pinOk((int)a[1], true) || !pinOk((int)a[2], true)) return 0;
              ok = w->begin((int)a[1], (int)a[2], argc > 2 ? (uint32_t)a[3] : 0);
            }
            wireUsed[b] = true;
            if (!ok) return nfail("no se pudo iniciar el bus I2C (revisa los pines SDA y SCL)");
            return 1;
          }
          case NF_WIRE_END: wireUsed[b] = false; return w->end();
          case NF_WIRE_CLOCK: w->setClock((uint32_t)a[1]); return 0;
          case NF_WIRE_GETCLOCK: return w->getClock();
          case NF_WIRE_BEGINTX: w->beginTransmission((uint8_t)a[1]); return 0;
          case NF_WIRE_WRITE: return (double)w->write((uint8_t)((int)a[1] & 0xFF));
          case NF_WIRE_ENDTX: return n > 1 ? w->endTransmission(a[1] != 0) : w->endTransmission();
          case NF_WIRE_REQUEST:
            return (double)w->requestFrom((uint8_t)a[1], (size_t)a[2], n > 3 ? a[3] != 0 : true);
          case NF_WIRE_AVAILABLE: return w->available();
          case NF_WIRE_READ: return w->read();
          default: return w->peek();
        }
      }

      // ---------------- SPI ----------------
      case NF_SPI_BEGIN: {
        int8_t p[4] = {-1, -1, -1, -1};
        for (int k = 0; k < n && k < 4; k++) {
          p[k] = (int8_t)a[k];
          if (p[k] >= 0 && !pinOk(p[k], k != 1)) return 0;
        }
        SPI.begin(p[0], p[1], p[2], p[3]);
        spiUsed = true;
        return 0;
      }
      case NF_SPI_END: SPI.end(); spiUsed = false; return 0;
      case NF_SPI_BEGINTX: {
        int64_t v = (int64_t)a[0];
        uint32_t clk = (uint32_t)(v & 0xFFFFFFFFLL);
        uint8_t order = (uint8_t)((v >> 32) & 0xFF);
        uint8_t mode = (uint8_t)((v >> 40) & 0xFF);
        if (clk == 0) clk = 1000000;
        SPI.beginTransaction(SPISettings(clk, order ? SPI_MSBFIRST : SPI_LSBFIRST, mode));
        return 0;
      }
      case NF_SPI_ENDTX: SPI.endTransaction(); return 0;
      case NF_SPI_TRANSFER: return SPI.transfer((uint8_t)((int)a[0] & 0xFF));
      case NF_SPI_TRANSFER16: return SPI.transfer16((uint16_t)((int64_t)a[0] & 0xFFFF));
      case NF_SPI_TRANSFER32: return (double)SPI.transfer32((uint32_t)((int64_t)a[0] & 0xFFFFFFFFLL));
      case NF_SPI_FREQ: SPI.setFrequency((uint32_t)a[0]); return 0;
      case NF_SPI_MODE: SPI.setDataMode((uint8_t)a[0]); return 0;
      case NF_SPI_ORDER: SPI.setBitOrder((uint8_t)a[0] ? SPI_MSBFIRST : SPI_LSBFIRST); return 0;
      case NF_SPI_WRITE: SPI.write((uint8_t)((int)a[0] & 0xFF)); return 0;
      case NF_SPI_WRITE16: SPI.write16((uint16_t)((int64_t)a[0] & 0xFFFF)); return 0;
      case NF_SPI_WRITE32: SPI.write32((uint32_t)((int64_t)a[0] & 0xFFFFFFFFLL)); return 0;

      // ---------------- Servo (PWM a 50 Hz) ----------------
      case NF_SERVO_ATTACH: {
        int h = (int)a[0];
        int pin = (int)a[1];
        int mn = n > 2 ? (int)a[2] : 544, mx = n > 3 ? (int)a[3] : 2400;
        if (mn < 100 || mx > 3000 || mn >= mx) return nfail("los limites del servo deben ir de 100 a 3000 us (min < max)");
        int i = (h >= 1 && h <= 16) ? h - 1 : -1;
        if (i >= 0 && servos[i].pin >= 0 && servos[i].pin != pin) releaseLedc(servos[i].pin);
        if (i < 0) {
          for (int k = 0; k < 16 && i < 0; k++)
            if (servos[k].pin < 0) i = k;
          if (i < 0) return nfail("como maximo 16 servos");
        }
        stopDds(pin);
        releaseLedc(pin);
        markOut(pin);
        if (!::ledcAttach((uint8_t)pin, 50, XRT_LEDC_BITS))
          return nfail("no quedan canales PWM libres para el servo del pin %d", pin);
        servos[i].pin = (int8_t)pin;
        servos[i].minUs = (uint16_t)mn;
        servos[i].maxUs = (uint16_t)mx;
        servos[i].us = 0;
        return i + 1;
      }
      case NF_SERVO_WRITE: case NF_SERVO_WRITE_US: {
        ServoCh* s = servoOf(a[0]);
        if (!s) return 0;
        double v = a[1];
        if (fn == NF_SERVO_WRITE && v < s->minUs) {   // como la libreria Servo: < min es un angulo
          if (v < 0) v = 0;
          if (v > 180) v = 180;
          v = s->minUs + (s->maxUs - s->minUs) * v / 180.0;
        }
        servoWriteUs(*s, v);
        return 0;
      }
      case NF_SERVO_READ: {
        ServoCh* s = servoOf(a[0]);
        if (!s) return 0;
        if (s->us == 0) return 90;
        return (double)lround((s->us - s->minUs) * 180.0 / (s->maxUs - s->minUs));
      }
      case NF_SERVO_READ_US: {
        ServoCh* s = servoOf(a[0]);
        if (!s) return 0;
        return s->us ? s->us : 1500;
      }
      case NF_SERVO_ATTACHED: {
        int i = (int)a[0] - 1;
        return i >= 0 && i < 16 && servos[i].pin >= 0;
      }
      case NF_SERVO_DETACH: {
        int i = (int)a[0] - 1;
        if (i >= 0 && i < 16 && servos[i].pin >= 0) {
          releaseLedc(servos[i].pin);
          servos[i].pin = -1;
        }
        return 0;
      }

      // ---------------- sistema ----------------
      case NF_ESP_RESTART: ESP.restart(); return 0;
      case NF_ESP_FREEHEAP: return ESP.getFreeHeap();
      case NF_ESP_HEAPSIZE: return ESP.getHeapSize();
      case NF_ESP_CPUFREQ: return getCpuFrequencyMhz();
      case NF_ESP_CORES: return ESP.getChipCores();
      case NF_ESP_FLASHSIZE: return ESP.getFlashChipSize();
      case NF_ESP_MAC: return (double)ESP.getEfuseMac();
      case NF_TEMPERATURE: return ::temperatureRead();
      case NF_AREAD_MV: return ::analogReadMilliVolts((uint8_t)a[0]);
      case NF_ASET_ATTEN:
        if (a[0] < 0 || a[0] > 3) return nfail("usa ADC_0db, ADC_2_5db, ADC_6db o ADC_11db");
        ::analogSetAttenuation((adc_attenuation_t)(int)a[0]);
        return 0;
      case NF_ASET_PIN_ATTEN:
        if (a[1] < 0 || a[1] > 3) return nfail("usa ADC_0db, ADC_2_5db, ADC_6db o ADC_11db");
        ::analogSetPinAttenuation((uint8_t)a[0], (adc_attenuation_t)(int)a[1]);
        return 0;
      case NF_RGBLED: {
        int pin = (int)a[0];
        if (!isRgb(pin) && !pinOk(pin, true)) return 0;
#if SOC_RMT_SUPPORTED
        if (isRgb(pin)) rgbUsed = true;
        else if (pin < 64) { rgbPins |= 1ULL << pin; outPins |= 1ULL << pin; }
        ::rgbLedWrite((uint8_t)pin, (uint8_t)a[1], (uint8_t)a[2], (uint8_t)a[3]);
        return 0;
#else
        return nfail("este ESP32 no puede controlar LEDs RGB");
#endif
      }
      case NF_SET_CPUFREQ: {
        int mhz = (int)a[0];
        if (mhz < 80) return nfail("por debajo de 80 MHz dejan de funcionar el Bluetooth y el USB");
        if (!bootCpuMhz) bootCpuMhz = getCpuFrequencyMhz();
        return ::setCpuFrequencyMhz((uint32_t)mhz);
      }
      case NF_ESP_RANDOM: return (double)esp_random();

      // ---------------- registros del GPIO y secuencias ----------------
      case NF_GPIO_REG_WRITE:
        return gpioRegWrite((int)a[0], (uint32_t)(int64_t)a[1]);
      case NF_GPIO_REG_READ: {
        uint32_t addr;
        if (!gpioReg((int)a[0], addr)) return 0;
        return (double)REG_READ(addr);
      }
      case NF_PATTERN_STOP:
        stopPattern();
        return 0;
      case NF_PATTERN_FREQ: {
        double hz = a[0];
        if (!patTimer) return nfail("no hay ninguna secuencia en marcha (usa antes patternStart)");
        if (!(hz > 0) || hz > 100000) return nfail("pasosPorSegundo debe estar entre 0 y 100000");
        uint64_t alarm = patAlarm(hz);
        ::timerAlarm(patTimer, alarm, true, 0);
        return (double)patRes / (double)alarm;
      }

      // ---------------- extras de Arduino ----------------
      case NF_LEDC_INVERT: {
        int pin = (int)a[0];
        if (!ledcBus(pin)) return nfail("el pin %d no tiene PWM: usa antes ledcAttach()", pin);
        return ::ledcOutputInvert((uint8_t)pin, a[1] != 0);
      }
      case NF_SD_ATTACH: case NF_SD_WRITE: case NF_SD_DETACH: {
        int pin = (int)a[0];
#if SOC_SDM_SUPPORTED
        if (fn == NF_SD_ATTACH) {
          idfTakePin(pin, true);
          if (!::sigmaDeltaAttach((uint8_t)pin, (uint32_t)a[1])) return nfail("no se pudo usar sigma-delta en el pin %d", pin);
          return 1;
        }
        if (fn == NF_SD_WRITE) return ::sigmaDeltaWrite((uint8_t)pin, (uint8_t)((int)a[1] & 0xFF));
        return ::sigmaDeltaDetach((uint8_t)pin);
#else
        (void)pin;
        return nfail("este ESP32 no tiene modulador sigma-delta");
#endif
      }

      // ---------------- ESP-IDF: driver/ledc.h ----------------
      case NF_IDF_LEDC_TIMER: {
        int mode = (int)a[0], res = (int)a[1], tmr = (int)a[2];
        uint32_t f = (uint32_t)a[3];
        bool deconf = n > 5 && a[5] != 0;
        if (!idfMode(mode) || !idfTimer(tmr)) return 0;
        if (!deconf && (res < 1 || res >= LEDC_TIMER_BIT_MAX))
          return nfail("duty_resolution no valida (de LEDC_TIMER_1_BIT a LEDC_TIMER_%d_BIT)", LEDC_TIMER_BIT_MAX - 1);
        if (!deconf && f < 1) return nfail("freq_hz debe ser mayor que 0");
        ledc_timer_config_t c;
        memset(&c, 0, sizeof c);
        c.speed_mode = (ledc_mode_t)mode;
        c.duty_resolution = (ledc_timer_bit_t)res;
        c.timer_num = (ledc_timer_t)tmr;
        c.freq_hz = f;
        c.clk_cfg = (ledc_clk_cfg_t)(int)a[4];
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
        c.deconfigure = deconf;
#endif
        esp_err_t e = ledc_timer_config(&c);
        if (e != ESP_OK && !deconf) {
          char m[150];
          int len = snprintf(m, sizeof m, "[X-RTbox] ledc_timer_config: no se pueden %lu Hz con %d bits (baja freq_hz o duty_resolution)\r\n",
                             (unsigned long)f, res);
          write(m, (size_t)len);
        }
        return (double)e;
      }
      case NF_IDF_LEDC_CHANNEL: {
        int gpio = (int)a[0], mode = (int)a[1], ch = (int)a[2], tmr = (int)a[4];
        if (!pinOk(gpio, true) || !idfMode(mode) || !idfChan(ch) || !idfTimer(tmr)) return 0;
        idfTakePin(gpio, true);
        ledc_channel_config_t c;
        memset(&c, 0, sizeof c);
        c.gpio_num = gpio;
        c.speed_mode = (ledc_mode_t)mode;
        c.channel = (ledc_channel_t)ch;
        c.intr_type = (ledc_intr_type_t)(int)a[3];
        c.timer_sel = (ledc_timer_t)tmr;
        c.duty = (uint32_t)a[5];
        c.hpoint = (int)a[6];
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
        c.sleep_mode = (ledc_sleep_mode_t)(int)a[7];
#endif
        c.flags.output_invert = a[8] != 0 ? 1 : 0;
        esp_err_t e = ledc_channel_config(&c);
        if (e == ESP_OK) idfCh[mode] |= 1u << ch;
        return (double)e;
      }
      case NF_IDF_SET_DUTY: case NF_IDF_UPDATE_DUTY: case NF_IDF_SET_DUTY_UPDATE: case NF_IDF_SET_DUTY_HPOINT:
      case NF_IDF_GET_DUTY: case NF_IDF_GET_HPOINT: case NF_IDF_STOP: case NF_IDF_FADE_TIME: case NF_IDF_FADE_STEP:
      case NF_IDF_FADE_START: case NF_IDF_FADE_STOP: case NF_IDF_FADE_TIME_START: case NF_IDF_FADE_STEP_START: {
        int mode = (int)a[0], ch = (int)a[1];
        if (!idfMode(mode) || !idfChan(ch)) return 0;
        ledc_mode_t M = (ledc_mode_t)mode;
        ledc_channel_t C = (ledc_channel_t)ch;
        switch (fn) {
          case NF_IDF_SET_DUTY: return ledc_set_duty(M, C, (uint32_t)a[2]);
          case NF_IDF_UPDATE_DUTY: return ledc_update_duty(M, C);
          case NF_IDF_SET_DUTY_UPDATE: idfEnsureFade(); return ledc_set_duty_and_update(M, C, (uint32_t)a[2], (uint32_t)a[3]);
          case NF_IDF_SET_DUTY_HPOINT: return ledc_set_duty_with_hpoint(M, C, (uint32_t)a[2], (uint32_t)a[3]);
          case NF_IDF_GET_DUTY: return (double)ledc_get_duty(M, C);
          case NF_IDF_GET_HPOINT: return ledc_get_hpoint(M, C);
          case NF_IDF_STOP: return ledc_stop(M, C, (uint32_t)a[2]);
          case NF_IDF_FADE_TIME: idfEnsureFade(); return ledc_set_fade_with_time(M, C, (uint32_t)a[2], (int)a[3]);
          case NF_IDF_FADE_STEP: idfEnsureFade(); return ledc_set_fade_with_step(M, C, (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4]);
          case NF_IDF_FADE_START: idfEnsureFade(); return ledc_fade_start(M, C, (ledc_fade_mode_t)(int)a[2]);
          case NF_IDF_FADE_STOP:
#if SOC_LEDC_SUPPORT_FADE_STOP
            return ledc_fade_stop(M, C);
#else
            return ESP_ERR_NOT_SUPPORTED;   // el ESP32 clasico no puede parar un fade a medias
#endif
          case NF_IDF_FADE_TIME_START:
            idfEnsureFade();
            return ledc_set_fade_time_and_start(M, C, (uint32_t)a[2], (uint32_t)a[3], (ledc_fade_mode_t)(int)a[4]);
          default:
            idfEnsureFade();
            return ledc_set_fade_step_and_start(M, C, (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4], (ledc_fade_mode_t)(int)a[5]);
        }
      }
      case NF_IDF_SET_FREQ: case NF_IDF_GET_FREQ: case NF_IDF_TIMER_PAUSE: case NF_IDF_TIMER_RESUME: case NF_IDF_TIMER_RST: {
        int mode = (int)a[0], tmr = (int)a[1];
        if (!idfMode(mode) || !idfTimer(tmr)) return 0;
        ledc_mode_t M = (ledc_mode_t)mode;
        ledc_timer_t T = (ledc_timer_t)tmr;
        if (fn == NF_IDF_SET_FREQ) return ledc_set_freq(M, T, (uint32_t)a[2]);
        if (fn == NF_IDF_GET_FREQ) return (double)ledc_get_freq(M, T);
        if (fn == NF_IDF_TIMER_PAUSE) return ledc_timer_pause(M, T);
        if (fn == NF_IDF_TIMER_RESUME) return ledc_timer_resume(M, T);
        return ledc_timer_rst(M, T);
      }
      case NF_IDF_BIND: {
        int mode = (int)a[0], ch = (int)a[1], tmr = (int)a[2];
        if (!idfMode(mode) || !idfChan(ch) || !idfTimer(tmr)) return 0;
        return ledc_bind_channel_timer((ledc_mode_t)mode, (ledc_channel_t)ch, (ledc_timer_t)tmr);
      }
      case NF_IDF_SET_PIN: {
        int gpio = (int)a[0], mode = (int)a[1], ch = (int)a[2];
        if (!idfMode(mode) || !idfChan(ch)) return 0;
        idfTakePin(gpio, true);
        esp_err_t e = ledc_set_pin(gpio, (ledc_mode_t)mode, (ledc_channel_t)ch);
        if (e == ESP_OK) idfCh[mode] |= 1u << ch;
        return e;
      }
      case NF_IDF_FADE_INSTALL: {
        if (idfFade) return ESP_OK;
        esp_err_t e = ledc_fade_func_install((int)a[0]);
        if (e == ESP_OK) idfFade = true;
        else if (e == ESP_ERR_INVALID_STATE) e = ESP_OK;   // ya estaba instalado
        return e;
      }
      case NF_IDF_FADE_UNINSTALL:
        if (idfFade) ledc_fade_func_uninstall();
        idfFade = false;
        return 0;
      case NF_IDF_FIND_RES: {
        uint32_t src = (uint32_t)a[0], f = (uint32_t)a[1];
        if (!f || src < f) return 0;
        uint32_t div = src / f;
        int r = 31 - __builtin_clz(div);
        if (r >= LEDC_TIMER_BIT_MAX) r = LEDC_TIMER_BIT_MAX - 1;
        return r;
      }

      // ---------------- ESP-IDF: driver/gpio.h ----------------
      case NF_IDF_GPIO_CONFIG: {
        uint64_t mask = (uint64_t)a[0];
        int mode = (int)a[1];
        bool out = (mode & 2) != 0;
        if (!mask) return nfail("pin_bit_mask esta vacio: usa (1ULL << pin)");
        for (int pin = 0; pin < 64; pin++) {
          if (!((mask >> pin) & 1)) continue;
          if (!pinOk(pin, out)) return 0;
          idfTakePin(pin, out);
          ::pinMode(pin, out ? ((mode & 4) ? OUTPUT_OPEN_DRAIN : OUTPUT) : INPUT);   // para que digitalRead/Write tambien vayan
        }
        gpio_config_t c;
        memset(&c, 0, sizeof c);
        c.pin_bit_mask = mask;
        c.mode = (gpio_mode_t)mode;
        c.pull_up_en = (gpio_pullup_t)(int)a[2];
        c.pull_down_en = (gpio_pulldown_t)(int)a[3];
        c.intr_type = (gpio_int_type_t)(int)a[4];
        return gpio_config(&c);
      }
      case NF_IDF_GPIO_SET_LEVEL:
        markOut((int)a[0]);
        return gpio_set_level((gpio_num_t)(int)a[0], a[1] != 0 ? 1 : 0);
      case NF_IDF_GPIO_GET_LEVEL:
        return gpio_get_level((gpio_num_t)(int)a[0]);
      case NF_IDF_GPIO_SET_DIR: {
        int pin = (int)a[0], mode = (int)a[1];
        bool out = (mode & 2) != 0;
        if (out && !pinOk(pin, true)) return 0;
        idfTakePin(pin, out);
        ::pinMode(pin, out ? ((mode & 4) ? OUTPUT_OPEN_DRAIN : OUTPUT) : INPUT);
        return gpio_set_direction((gpio_num_t)pin, (gpio_mode_t)mode);
      }
      case NF_IDF_GPIO_RESET: idfTakePin((int)a[0], false); return gpio_reset_pin((gpio_num_t)(int)a[0]);
      case NF_IDF_GPIO_PULL: return gpio_set_pull_mode((gpio_num_t)(int)a[0], (gpio_pull_mode_t)(int)a[1]);
      case NF_IDF_GPIO_PU_EN: return gpio_pullup_en((gpio_num_t)(int)a[0]);
      case NF_IDF_GPIO_PU_DIS: return gpio_pullup_dis((gpio_num_t)(int)a[0]);
      case NF_IDF_GPIO_PD_EN: return gpio_pulldown_en((gpio_num_t)(int)a[0]);
      case NF_IDF_GPIO_PD_DIS: return gpio_pulldown_dis((gpio_num_t)(int)a[0]);
      case NF_ESP_TIMER_US: return (double)esp_timer_get_time();
      default:
        return nfail("esta funcion no esta disponible en el firmware");
    }
  }

  void endTimer(UserTimer& u) {
    if (!u.t) return;
    u.armed = false;
    int s = u.slot;
    u.slot = -1;
    ::timerEnd(u.t);
    u.t = nullptr;
    freeSlot(s);
  }

  // Deja los pines en un estado seguro al detener un programa
  void resetHardware() {
    // primero se paran todas las fuentes de interrupciones
    stopPattern();
    for (int c = 0; c < DDS_MAX; c++) dds[c].kind = DDS_OFF;
    if (ddsTimer) { ::timerEnd(ddsTimer); ddsTimer = nullptr; }
    for (int c = 0; c < DDS_MAX; c++) dds[c].pin = -1;
    for (int k = 0; k < MAX_UTIMERS; k++) endTimer(utm[k]);
    for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
      if (pinIrq[p].active) ::detachInterrupt((uint8_t)p);
      pinIrq[p].active = false;
      pinIrq[p].counter = false;
      pinIrq[p].slot = -1;
      pinIrq[p].count = 0;
    }
    slotUsed = 0;
    portENTER_CRITICAL(&evMux);
    for (int s = 0; s < 32; s++) evCount[s] = 0;
    eventBits = 0;
    portEXIT_CRITICAL(&evMux);
    if (evSem) xSemaphoreTake(evSem, 0);
    for (int b = 0; b < 2; b++) {
      TwoWire* w = wireBus(b);
      if (wireUsed[b] && w) w->end();
      wireUsed[b] = false;
    }
    if (spiUsed) SPI.end();
    spiUsed = false;
    // canales LEDC configurados con la API de ESP-IDF
    for (int m = 0; m < 2; m++)
      for (int c = 0; c < 32; c++)
        if (idfCh[m] & (1u << c)) ledc_stop((ledc_mode_t)m, (ledc_channel_t)c, 0);
    idfCh[0] = idfCh[1] = 0;
    if (idfFade) ledc_fade_func_uninstall();
    idfFade = false;
#if SOC_RMT_SUPPORTED
    for (int p = 0; p < 64; p++)
      if (rgbPins & (1ULL << p)) ::rgbLedWrite((uint8_t)p, 0, 0, 0);
#endif
    for (int p = 0; p < 64; p++) {
      uint64_t b = 1ULL << p;
      if (!(outPins & b)) continue;
      if (tonePins & b) ::noTone(p);
      if (pwmPins & b) ::analogWrite(p, 0);
      ::pinMode(p, INPUT);   // tambien suelta PWM, DAC y sigma-delta
    }
#ifdef RGB_BUILTIN
    if (rgbUsed) ::digitalWrite(RGB_BUILTIN, LOW);
#endif
    if (bootCpuMhz && getCpuFrequencyMhz() != bootCpuMhz) ::setCpuFrequencyMhz(bootCpuMhz);
    outPins = pwmPins = tonePins = rgbPins = 0;
    rgbUsed = false;
    clearTables();
  }
};

static EspHal hal;
static minic::Interpreter interp(hal);

// ---------------------------------------------------------------------
//  Protocolo (igual por Bluetooth y por USB)
// ---------------------------------------------------------------------
static void handleIncoming(const uint8_t* d, size_t n, RxState& rx) {
  if (n == 0) return;
  char op = (char)d[0];
  const char* p = (const char*)d + 1;
  size_t len = n - 1;
  switch (op) {
    case 'B': {  // B<longitud>|<crc hex>|<nombre>
      std::string hdr(p, len);
      size_t a = hdr.find('|');
      size_t b = a == std::string::npos ? a : hdr.find('|', a + 1);
      if (a == std::string::npos || b == std::string::npos) { sendCtl('!', "Cabecera de programa no valida"); return; }
      rx.expected = (size_t)strtoul(hdr.substr(0, a).c_str(), nullptr, 10);
      rx.crc = (uint32_t)strtoul(hdr.substr(a + 1, b - a - 1).c_str(), nullptr, 16);
      rx.name = hdr.substr(b + 1);
      if (rx.name.size() > 46) rx.name.resize(46);
      if (rx.expected == 0 || rx.expected > MAX_PROGRAM) {
        sendCtl('!', "El programa es demasiado grande (max %u bytes)", (unsigned)MAX_PROGRAM);
        rx.active = false;
        return;
      }
      rx.buf.clear();
      rx.buf.reserve(rx.expected);
      rx.active = true;
      return;
    }
    case 'D':
      if (!rx.active) return;
      if (rx.buf.size() + len > rx.expected) {
        rx.active = false;
        rx.buf.clear();
        sendCtl('!', "Se recibieron mas datos de los esperados");
        return;
      }
      rx.buf.append(p, len);
      return;
    case 'E': {
      if (!rx.active) { sendCtl('!', "Fin de programa sin inicio"); return; }
      rx.active = false;
      if (rx.buf.size() != rx.expected) {
        sendCtl('!', "Programa incompleto (%u de %u bytes). Intentalo de nuevo", (unsigned)rx.buf.size(), (unsigned)rx.expected);
        rx.buf.clear();
        return;
      }
      if (crc32((const uint8_t*)rx.buf.data(), rx.buf.size()) != rx.crc) {
        sendCtl('!', "El programa llego danado (CRC). Intentalo de nuevo");
        rx.buf.clear();
        return;
      }
      bool idle = false;
      if (!pendingClaim.compare_exchange_strong(idle, true)) {
        sendCtl('!', "Ocupado cargando otro programa");
        rx.buf.clear();
        return;
      }
      pendingCode.swap(rx.buf);
      pendingName = rx.name;
      rx.buf.clear();
      rx.buf.shrink_to_fit();
      pendingReady.store(true);
      interp.requestStop();
      sendCtl('K', "Recibido '%s' (%u bytes)", pendingName.c_str(), (unsigned)pendingCode.size());
      return;
    }
    case 'R': cmdRun.store(true); interp.requestStop(); return;
    case 'S': cmdStop.store(true); interp.requestStop(); return;
    case 'I': cmdInfo.store(true); return;
    case 'P': return;  // ping (mantiene viva la conexion USB)
    case 'M': {
      int m = atoi(std::string(p, len).c_str());
      if (m >= 23 && m <= 517) bleMtu.store(m);
      return;
    }
    case 'W': progInput.write(p, len); return;
    default: return;
  }
}

// ---------------------------------------------------------------------
//  USB (Serial): tramas  0x1E <tipo> <datos en base64> '\n'
//  Lo que no va en tramas es entrada normal del programa (Serial.read).
// ---------------------------------------------------------------------
static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

// Devuelve los bytes escritos o -1 si hay un error
static int b64decode(const char* in, size_t n, uint8_t* out, size_t max) {
  uint32_t acc = 0;
  int bits = 0;
  size_t w = 0;
  for (size_t i = 0; i < n; i++) {
    char c = in[i];
    if (c == '=') break;
    if (c == ' ' || c == '\r' || c == '\t') continue;
    int v = b64val(c);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (w >= max) return -1;
      out[w++] = (uint8_t)((acc >> bits) & 0xFF);
    }
  }
  return (int)w;
}

// Escribe 0x1E + texto + '\n' de una vez (no se mezcla con la salida del programa)
static void usbSendFrame(const char* text, size_t n) {
  static char buf[212];
  if (n > sizeof(buf) - 2) n = sizeof(buf) - 2;
  buf[0] = 0x1E;
  for (size_t i = 0; i < n; i++) {
    char ch = text[i];
    buf[i + 1] = (ch == '\n' || ch == '\r' || ch == 0x1E) ? ' ' : ch;
  }
  buf[n + 1] = '\n';
  Serial.write((const uint8_t*)buf, n + 2);
}

static void handleUsbFrame(const char* f, size_t n) {
  static uint8_t dec[840];
  if (n == 0) return;
  lastUsbFrame.store(millis());
  usbSeen.store(true);
  dec[0] = (uint8_t)f[0];
  int m = b64decode(f + 1, n - 1, dec + 1, sizeof(dec) - 1);
  if (m >= 0) handleIncoming(dec, (size_t)m + 1, usbRx);
  else sendCtl('!', "Trama USB danada");
  char ack[2] = {'A', f[0]};
  usbSendFrame(ack, 2);  // acuse de recibo (con el tipo): la app manda la siguiente
}

static void pollUsb() {
  static char line[1124];
  static size_t lineLen = 0;
  static bool inFrame = false;
  int budget = 4096;
  while (budget-- > 0 && Serial.available() > 0) {
    int c = Serial.read();
    if (c < 0) break;
    if (inFrame) {
      if (c == '\n') {
        inFrame = false;
        handleUsbFrame(line, lineLen);
        lineLen = 0;
      } else if (c == 0x1E) {
        lineLen = 0;  // empieza otra trama
      } else if (c != '\r') {
        if (lineLen < sizeof(line)) line[lineLen++] = (char)c;
        else inFrame = false;  // trama demasiado larga: se descarta
      }
    } else if (c == 0x1E) {
      inFrame = true;
      lineLen = 0;
    } else {
      char ch = (char)c;
      progInput.write(&ch, 1);
    }
  }
}

// ---------------------------------------------------------------------
//  Bluetooth
// ---------------------------------------------------------------------
class ServerCB : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    (void)s;
    bleMtu.store(23);
    consoleOut.clear();
    bleConnected.store(true);
    Serial.println("[X-RTbox] Movil conectado");
  }
  void onDisconnect(BLEServer* s) override {
    (void)s;
    bleConnected.store(false);
    bleRx.active = false;
    Serial.println("[X-RTbox] Movil desconectado");
  }
};

class RxCB : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    // getValue() es std::string en el core 2.x y String en el 3.x;
    // ambos tienen c_str() y length(). El protocolo es solo texto.
    auto v = c->getValue();
    handleIncoming((const uint8_t*)v.c_str(), v.length(), bleRx);
  }
};

// Tarea de comunicacion: lee el USB y envia al movil (Bluetooth y/o USB)
// los mensajes y la salida del programa
static void commTask(void*) {
  static uint8_t frame[520];
  bool wasConnected = false;
  uint32_t tick = 0;
  for (;;) {
    pollUsb();
    bool conn = bleConnected.load();
    bool usb = usbAppActive();
    if (cmdInfo.exchange(false)) sendState();
    CtlMsg m;
    while (xQueueReceive(ctlQueue, &m, 0) == pdTRUE) {
      size_t n = strlen(m.text);
      if (usb) usbSendFrame(m.text, n);
      if (conn) {
        size_t maxp = (size_t)bleMtu.load() - 3;
        pTx->setValue((uint8_t*)m.text, n > maxp ? maxp : n);
        pTx->notify();
        vTaskDelay(pdMS_TO_TICKS(4));
      }
    }
    // la salida del programa ya va directa por USB; por Bluetooth se envia aqui
    if (conn && (tick % 3) == 0) {
      size_t chunk = (size_t)bleMtu.load() - 4;
      if (chunk > 500) chunk = 500;
      for (int k = 0; k < 3; k++) {
        size_t n = consoleOut.read((char*)frame + 1, chunk);
        if (!n) break;
        frame[0] = 'L';
        pTx->setValue(frame, n + 1);
        pTx->notify();
        vTaskDelay(pdMS_TO_TICKS(4));
      }
      if (consoleOut.takeLost()) {
        const char* lost = "L\r\n[...se perdio salida: el programa imprime demasiado rapido...]\r\n";
        pTx->setValue((uint8_t*)lost, strlen(lost));
        pTx->notify();
      }
    }
    if (!conn && wasConnected) {
      vTaskDelay(pdMS_TO_TICKS(300));
      BLEDevice::startAdvertising();
      Serial.println("[X-RTbox] Esperando conexion Bluetooth...");
    }
    wasConnected = conn;
    tick++;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void setupBle() {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(devName, sizeof devName, "X-RTbox-%04X", (unsigned)((mac >> 32) & 0xFFFF));
  BLEDevice::init(devName);
  BLEDevice::setMTU(517);
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ServerCB());
  BLEService* svc = pServer->createService(SERVICE_UUID);
  BLECharacteristic* rx = svc->createCharacteristic(
      RX_UUID, BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  rx->setCallbacks(new RxCB());
  pTx = svc->createCharacteristic(TX_UUID, BLECharacteristic::PROPERTY_NOTIFY);
#if XRTBOX_ADD_2902
  pTx->addDescriptor(new BLE2902());
#endif
  svc->start();
  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->setScanResponse(true);
  BLEDevice::startAdvertising();
  Serial.printf("[X-RTbox] Bluetooth listo: %s\n", devName);
}

// ---------------------------------------------------------------------
//  Programas
// ---------------------------------------------------------------------
static void handleResult(minic::RunResult r) {
  switch (r) {
    case minic::RUN_OK:
      return;
    case minic::RUN_FINISHED:
      progState.store(ST_FIN);
      sendCtl('K', "Programa terminado (no tiene loop)");
      sendState();
      return;
    case minic::RUN_STOPPED:
      progState.store(ST_STOP);
      hal.resetHardware();
      if (!pendingReady.load() && !cmdRun.load()) {
        sendCtl('K', "Programa detenido");
        sendState();
      }
      return;
    case minic::RUN_ERROR:
    default:
      progState.store(ST_ERR);
      hal.resetHardware();
      sendCtl('!', "Error de ejecucion: %s", interp.error().c_str());
      sendState();
      return;
  }
}

static void loadAndStart(const std::string& code, const char* name) {
  setProgName(name);
  hal.resetHardware();
  interp.unload();
  progInput.clear();
  if (!interp.load(code.data(), code.size())) {
    progState.store(ST_ERR);
    sendCtl('!', "Error de compilacion: %s", interp.error().c_str());
    sendState();
    return;
  }
  progState.store(ST_RUN);
  sendCtl('K', "Ejecutando '%s'", name);
  sendState();
  handleResult(interp.start());
}

static bool saveProgram(const std::string& code, const std::string& name) {
  File f = LittleFS.open(PROG_FILE, "w");
  if (!f) return false;
  size_t w = f.write((const uint8_t*)code.data(), code.size());
  f.close();
  File g = LittleFS.open(NAME_FILE, "w");
  if (g) { g.print(name.c_str()); g.close(); }
  return w == code.size();
}

static bool loadStored(std::string& code, std::string& name) {
  if (!LittleFS.exists(PROG_FILE)) return false;
  File f = LittleFS.open(PROG_FILE, "r");
  if (!f) return false;
  size_t sz = f.size();
  if (sz == 0 || sz > MAX_PROGRAM) { f.close(); return false; }
  code.resize(sz);
  size_t r = f.read((uint8_t*)&code[0], sz);
  f.close();
  if (r != sz) return false;
  name = "programa";
  File g = LittleFS.open(NAME_FILE, "r");
  if (g) {
    String n = g.readString();
    g.close();
    if (n.length()) name = n.c_str();
  }
  return true;
}

static void runStored() {
  std::string code, name;
  if (!loadStored(code, name)) {
    progState.store(ST_NONE);
    sendCtl('!', "No hay ningun programa guardado en el ESP32");
    sendState();
    return;
  }
  loadAndStart(code, name.c_str());
}

// Bucle principal del interprete (tarea propia, pila de 40 KB)
static void vmStep() {
  if (pendingReady.load()) {
    std::string code;
    code.swap(pendingCode);
    std::string name = pendingName;
    pendingReady.store(false);
    pendingClaim.store(false);
    cmdStop.store(false);
    cmdRun.store(false);
    if (!saveProgram(code, name)) sendCtl('!', "Aviso: no se pudo guardar el programa en la memoria");
    loadAndStart(code, name.c_str());
    return;
  }
  if (cmdStop.exchange(false)) {
    int st = progState.load();
    if (st == ST_RUN || st == ST_FIN) {
      progState.store(ST_STOP);
      hal.resetHardware();
      sendCtl('K', "Programa detenido");
    }
    sendState();
  }
  if (cmdRun.exchange(false)) {
    runStored();
    return;
  }
  if (progState.load() == ST_RUN) {
    handleResult(interp.loopOnce());
  } else {
    delay(10);
  }
}

static void vmTask(void* arg) {
  bool safeBoot = arg != nullptr;
  evSem = xSemaphoreCreateBinary();
  if (safeBoot) {
    Serial.println("[X-RTbox] Arranque seguro (BOOT pulsado): no se ejecuta el programa guardado");
  } else {
    std::string code, name;
    if (loadStored(code, name)) {
      Serial.printf("[X-RTbox] Ejecutando programa guardado '%s'\n", name.c_str());
      loadAndStart(code, name.c_str());
    } else {
      Serial.println("[X-RTbox] No hay programa guardado. Mandalo desde la app.");
    }
  }
  for (;;) vmStep();
}

// ---------------------------------------------------------------------
//  setup / loop
// ---------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(SAFE_BOOT_PIN, INPUT_PULLUP);
  delay(5);
  bool safeBoot = digitalRead(SAFE_BOOT_PIN) == LOW;
  pinMode(SAFE_BOOT_PIN, INPUT);

  Serial.println();
  Serial.println("[X-RTbox] ==== X-RTbox ESP32 ====");
  Serial.printf("[X-RTbox] Firmware %s\n", XRTBOX_FW_VERSION);
  if (!LittleFS.begin(true)) Serial.println("[X-RTbox] Aviso: no se pudo montar LittleFS (no se guardaran programas)");

  ctlQueue = xQueueCreate(12, sizeof(CtlMsg));
  setupBle();
  xTaskCreatePinnedToCore(commTask, "xrtbox_comm", 6144, nullptr, 2, nullptr, 0);
  Serial.printf("[X-RTbox] Memoria libre: %u bytes\n", (unsigned)ESP.getFreeHeap());
  if (xTaskCreatePinnedToCore(vmTask, "xrtbox_vm", VM_STACK, safeBoot ? (void*)1 : nullptr, 1, nullptr,
                              ARDUINO_RUNNING_CORE) != pdPASS) {
    Serial.println("[X-RTbox] ERROR: no hay memoria para el interprete");
  }
}

void loop() {
  // todo el trabajo lo hacen las tareas xrtbox_vm y xrtbox_comm
  vTaskDelay(pdMS_TO_TICKS(1000));
}
