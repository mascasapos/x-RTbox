// =====================================================================
//  MiniC - Interprete de C++ estilo Arduino para ESP32
//  Parte del proyecto X-RTbox.
//
//  Ejecuta programas escritos en un subconjunto de C/C++ de Arduino
//  (setup/loop, variables, arrays, funciones, if/for/while/switch,
//  String, Serial, pinMode, digitalWrite, analogRead, delay, ...)
//  sin necesidad de recompilar el firmware.
//
//  El acceso al hardware se hace a traves de la clase Hal, asi el
//  interprete se puede compilar y probar tambien en un PC.
// =====================================================================
#ifndef MINIC_H
#define MINIC_H

#include <stdint.h>
#include <stddef.h>
#include <string>
#include <atomic>

namespace minic {

// Valores de modo de pin que ve el programa (iguales a los del core ESP32).
enum PinModeConst {
  MC_INPUT = 0x01,
  MC_OUTPUT = 0x03,
  MC_INPUT_PULLUP = 0x05,
  MC_INPUT_PULLDOWN = 0x09,
  MC_OUTPUT_OPEN_DRAIN = 0x13
};

// Funciones nativas del ESP32 (las implementa el firmware en Hal::native).
// Los argumentos llegan como double y el resultado se devuelve como double.
enum NativeFn {
  NF_NONE = 0,
  // PWM / LEDC
  NF_LEDC_ATTACH, NF_LEDC_ATTACH_CH, NF_LEDC_SETUP, NF_LEDC_ATTACH_PIN, NF_LEDC_DETACH, NF_LEDC_WRITE,
  NF_LEDC_TONE, NF_LEDC_NOTE, NF_LEDC_READ, NF_LEDC_READ_FREQ, NF_LEDC_CHANGE_FREQ, NF_LEDC_FADE,
  NF_AW_FREQ, NF_AW_RES,
  // generadores de senal (X-RTbox)
  NF_SIG_SINE, NF_SIG_TRIANGLE, NF_SIG_SAW, NF_SIG_SQUARE, NF_SIG_STOP, NF_DAC_DISABLE,
  // medida
  NF_CNT_START, NF_CNT_READ, NF_CNT_RESET, NF_CNT_STOP, NF_PULSEIN_LONG,
  // interrupciones y temporizadores (los avisos llegan por Hal::eventBits)
  NF_PIN_EVENT_ATTACH, NF_PIN_EVENT_DETACH,
  NF_TIMER_BEGIN, NF_TIMER_ATTACH, NF_TIMER_DETACH, NF_TIMER_ALARM, NF_TIMER_ALARM_WRITE,
  NF_TIMER_ALARM_ENABLE, NF_TIMER_ALARM_DISABLE, NF_TIMER_START, NF_TIMER_STOP, NF_TIMER_RESTART,
  NF_TIMER_END, NF_TIMER_READ, NF_TIMER_READ_US, NF_TIMER_READ_MS, NF_TIMER_READ_S, NF_TIMER_WRITE,
  // I2C (primer argumento: bus 0 = Wire, 1 = Wire1)
  NF_WIRE_BEGIN, NF_WIRE_END, NF_WIRE_CLOCK, NF_WIRE_GETCLOCK, NF_WIRE_BEGINTX, NF_WIRE_WRITE,
  NF_WIRE_ENDTX, NF_WIRE_REQUEST, NF_WIRE_AVAILABLE, NF_WIRE_READ, NF_WIRE_PEEK,
  // SPI
  NF_SPI_BEGIN, NF_SPI_END, NF_SPI_BEGINTX, NF_SPI_ENDTX, NF_SPI_TRANSFER, NF_SPI_TRANSFER16,
  NF_SPI_TRANSFER32, NF_SPI_FREQ, NF_SPI_MODE, NF_SPI_ORDER, NF_SPI_WRITE, NF_SPI_WRITE16, NF_SPI_WRITE32,
  // Servo (primer argumento: identificador del servo)
  NF_SERVO_ATTACH, NF_SERVO_WRITE, NF_SERVO_WRITE_US, NF_SERVO_READ, NF_SERVO_READ_US,
  NF_SERVO_ATTACHED, NF_SERVO_DETACH,
  // sistema
  NF_ESP_RESTART, NF_ESP_FREEHEAP, NF_ESP_HEAPSIZE, NF_ESP_CPUFREQ, NF_ESP_CORES, NF_ESP_FLASHSIZE,
  NF_ESP_MAC, NF_TEMPERATURE, NF_AREAD_MV, NF_ASET_ATTEN, NF_ASET_PIN_ATTEN, NF_RGBLED,
  NF_SET_CPUFREQ, NF_ESP_RANDOM, NF_ESP_TIMER_US,
  NF_LEDC_INVERT, NF_SD_ATTACH, NF_SD_WRITE, NF_SD_DETACH,
  // ESP-IDF (driver/ledc.h, driver/gpio.h); las estructuras llegan campo a campo
  NF_IDF_LEDC_TIMER, NF_IDF_LEDC_CHANNEL, NF_IDF_SET_DUTY, NF_IDF_UPDATE_DUTY, NF_IDF_SET_DUTY_UPDATE,
  NF_IDF_SET_DUTY_HPOINT, NF_IDF_GET_DUTY, NF_IDF_GET_HPOINT, NF_IDF_SET_FREQ, NF_IDF_GET_FREQ, NF_IDF_STOP,
  NF_IDF_TIMER_PAUSE, NF_IDF_TIMER_RESUME, NF_IDF_TIMER_RST, NF_IDF_BIND, NF_IDF_SET_PIN, NF_IDF_FADE_INSTALL,
  NF_IDF_FADE_UNINSTALL, NF_IDF_FADE_TIME, NF_IDF_FADE_STEP, NF_IDF_FADE_START, NF_IDF_FADE_STOP,
  NF_IDF_FADE_TIME_START, NF_IDF_FADE_STEP_START, NF_IDF_FIND_RES,
  NF_IDF_GPIO_CONFIG, NF_IDF_GPIO_SET_LEVEL, NF_IDF_GPIO_GET_LEVEL, NF_IDF_GPIO_SET_DIR, NF_IDF_GPIO_RESET,
  NF_IDF_GPIO_PULL, NF_IDF_GPIO_PU_EN, NF_IDF_GPIO_PU_DIS, NF_IDF_GPIO_PD_EN, NF_IDF_GPIO_PD_DIS,
  // registros del GPIO (GPIO.out_w1ts, REG_WRITE...) y reproductor de secuencias
  NF_GPIO_REG_WRITE, NF_GPIO_REG_READ, NF_PATTERN_STOP, NF_PATTERN_FREQ,
  NF__COUNT
};

// Funciones nativas que devuelven texto (Hal::nativeStr)
enum NativeStrFn { NS_CHIP_MODEL = 1, NS_SDK_VERSION };

// Capa de acceso al hardware. El firmware del ESP32 la implementa con las
// funciones reales de Arduino; los tests de PC la implementan simulada.
class Hal {
public:
  virtual ~Hal() {}
  virtual void pinMode(int pin, int mode) = 0;
  virtual void digitalWrite(int pin, int val) = 0;
  virtual int digitalRead(int pin) = 0;
  virtual int analogRead(int pin) = 0;
  virtual void analogWrite(int pin, int val) = 0;
  virtual void analogReadResolution(int bits) { (void)bits; }
  virtual void dacWrite(int pin, int val) { (void)pin; (void)val; }
  virtual int touchRead(int pin) { (void)pin; return 0; }
  virtual void tone(int pin, unsigned freq, unsigned long dur) { (void)pin; (void)freq; (void)dur; }
  virtual void noTone(int pin) { (void)pin; }
  virtual unsigned long pulseIn(int pin, int state, unsigned long timeoutUs) {
    (void)pin; (void)state; (void)timeoutUs; return 0;
  }
  virtual uint32_t millis() = 0;
  virtual uint32_t micros() = 0;
  virtual void delayMs(uint32_t ms) = 0;   // el interprete la llama en trozos cortos
  virtual void delayUs(uint32_t us) = 0;
  // Salida de Serial.print (se reenvia al USB y al movil).
  virtual void write(const char* s, size_t n) = 0;
  // Entrada de Serial (datos que llegan del USB o del movil).
  virtual int available() { return 0; }
  virtual int read() { return -1; }
  virtual int peek() { return -1; }
  // Se llama de vez en cuando para dejar respirar al sistema.
  virtual void yield() {}
  // Devuelve un mensaje si el pin no se puede usar (p.ej. pines de la flash).
  virtual const char* pinError(int pin, bool output) { (void)pin; (void)output; return nullptr; }
  virtual uint32_t entropy() { return 0x12345678u; }
  virtual int ledBuiltin() { return 2; }
  // Nombres de pines de la placa (SDA, SCL, MOSI, DAC1...). -1 si no existe.
  virtual int pinConstant(const std::string& name) { (void)name; return -1; }

  // Interrupciones y temporizadores: el firmware pone a 1 el bit del evento
  // (desde una interrupcion) y el interprete llama a la funcion del programa.
  volatile uint32_t eventBits = 0;
  virtual uint32_t takeEvents() { uint32_t b = eventBits; eventBits = 0; return b; }
  // Cuantas veces ha ocurrido el evento desde la ultima consulta (0 = ninguna).
  virtual uint32_t takeEventCount(int slot) { (void)slot; return 1; }

  // Funciones nativas. Si algo falla, el firmware deja el motivo en nativeError.
  std::string nativeError;
  virtual double native(int fn, const double* a, int argc) {
    (void)fn; (void)a; (void)argc;
    nativeError = "esta funcion no esta disponible aqui";
    return 0;
  }
  virtual std::string nativeStr(int fn) { (void)fn; return std::string(); }
  // Reproduce en bucle una secuencia sobre varios pines (bit k del paso -> pins[k]) a stepHz pasos/s.
  virtual double startPattern(const int* pins, int npins, const uint8_t* seq, int len, double stepHz) {
    (void)pins; (void)npins; (void)seq; (void)len; (void)stepHz;
    nativeError = "esta funcion no esta disponible aqui";
    return 0;
  }
};

enum RunResult { RUN_OK = 0, RUN_ERROR, RUN_STOPPED, RUN_FINISHED };

struct Impl;

class Interpreter {
public:
  explicit Interpreter(Hal& hal);
  ~Interpreter();

  // Analiza el codigo fuente. Devuelve false si hay errores (ver error()).
  bool load(const char* src, size_t len);
  // Inicializa variables globales y ejecuta setup().
  RunResult start();
  // Ejecuta loop() una vez. RUN_FINISHED si el programa no tiene loop().
  RunResult loopOnce();
  // Libera el programa cargado.
  void unload();

  bool isLoaded() const;
  bool hasLoop() const;
  size_t programBytes() const;   // memoria usada por el programa analizado

  // Se puede llamar desde otra tarea/hilo para detener el programa.
  void requestStop() { stopReq.store(true); }
  bool stopRequested() const { return stopReq.load(); }

  const std::string& error() const { return err; }

private:
  Interpreter(const Interpreter&);
  Interpreter& operator=(const Interpreter&);
  Hal& hal;
  Impl* impl;
  std::string err;
  std::atomic<bool> stopReq;
};

}  // namespace minic

#endif
