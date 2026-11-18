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
