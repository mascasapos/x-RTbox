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
//  Core: "esp32" de Espressif 3.3.x (recomendado) o 2.0.x en el Arduino IDE.
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
#include "MiniC.h"

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
  sendCtl('T', "%s|%s|%u", names[st < 0 || st > 4 ? 0 : st], n, (unsigned)ESP.getFreeHeap());
}

// ---------------------------------------------------------------------
//  Hardware para el interprete
// ---------------------------------------------------------------------
class EspHal : public minic::Hal {
 public:
  uint64_t outPins = 0, pwmPins = 0, tonePins = 0;
  bool rgbUsed = false;
  uint32_t lastYield = 0;
  char perr[80];

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
    if (pin >= 6 && pin <= 11) { snprintf(perr, sizeof perr, "el pin %d esta reservado para la memoria flash", pin); return perr; }
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
    if (pin >= 0 && pin < 64) { pwmPins |= (1ULL << pin); outPins |= (1ULL << pin); }
    ::analogWrite(pin, v < 0 ? 0 : v);
  }
  void analogReadResolution(int bits) override { ::analogReadResolution(bits); }
  void dacWrite(int pin, int v) override {
#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
    ::dacWrite(pin, (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)));
#else
    (void)pin; (void)v;
    write("[X-RTbox] dacWrite no existe en este ESP32\r\n", 44);
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
  void delayMs(uint32_t ms) override { ::delay(ms); lastYield = ::millis(); }
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

  // Deja los pines en un estado seguro al detener un programa
  void resetHardware() {
    for (int p = 0; p < 64; p++) {
      uint64_t b = 1ULL << p;
      if (!(outPins & b)) continue;
      if (tonePins & b) ::noTone(p);
      if (pwmPins & b) ::analogWrite(p, 0);
      ::pinMode(p, INPUT);
    }
#ifdef RGB_BUILTIN
    if (rgbUsed) ::digitalWrite(RGB_BUILTIN, LOW);
#endif
    outPins = pwmPins = tonePins = 0;
    rgbUsed = false;
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
