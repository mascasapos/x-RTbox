// =====================================================================
//  MiniC - Interprete de C++ estilo Arduino (implementacion)
//  Proyecto X-RTbox
// =====================================================================
#include "MiniC.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>
#include <ctype.h>
#include <map>
#include <deque>
#include <vector>
#include <utility>

#if defined(__GNUC__)
#define NOINLINE __attribute__((noinline))
#else
#define NOINLINE
#endif

namespace minic {

// ---------------------------------------------------------------------
//  Limites (pensados para la RAM del ESP32)
// ---------------------------------------------------------------------
static const int STACK_SLOTS = 1024;     // variables locales en total
static const int MAX_CALL_DEPTH = 40;    // profundidad de llamadas (recursion)
static const int MAX_PARSE_NEST = 32;    // anidamiento de expresiones/bloques
static const int MAX_ARRAY_ELEMS = 4096; // elementos por array
static const int MAX_ARGS = 10;          // argumentos por llamada

// ---------------------------------------------------------------------
//  Tipos declarados
// ---------------------------------------------------------------------
enum Ty : uint8_t {
  TY_VOID, TY_BOOL, TY_CHAR, TY_U8, TY_I8, TY_I16, TY_U16, TY_I32, TY_U32,
  TY_I64, TY_U64, TY_FLOAT, TY_DOUBLE, TY_STRING, TY_ANY, TY_SERVO, TY_SPISET, TY__COUNT
};
static const uint8_t kTyBits[TY__COUNT] = {0, 1, 8, 8, 8, 16, 16, 32, 32, 64, 64, 0, 0, 0, 0, 32, 64};
static const bool kTyUns[TY__COUNT] = {false, true, false, true, false, false, true, false, true,
                                       false, true, false, false, false, false, false, false};
static const uint8_t kTySize[TY__COUNT] = {0, 1, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 12, 4, 4, 8};
static inline bool tyIsInt(uint8_t t) { return t >= TY_BOOL && t <= TY_U64; }
// estructuras: el tipo TY_STRUCT0 + n es la estructura numero n
static const uint8_t TY_STRUCT0 = 32;
static inline bool tyIsStruct(uint8_t t) { return t >= TY_STRUCT0; }

// ---------------------------------------------------------------------
//  Valores en tiempo de ejecucion
// ---------------------------------------------------------------------
enum VT : uint8_t { V_VOID, V_INT, V_FLT, V_STR, V_ARR, V_REF, V_FUNC };

struct StrObj;
struct ArrObj;

struct Value {
  uint8_t t;
  uint8_t isChar;
  union {
    int64_t i;
    double f;
    StrObj* s;
    ArrObj* a;
    Value* r;
  };
  Value() : t(V_VOID), isChar(0), i(0) {}
  Value(const Value& o) : t(o.t), isChar(o.isChar), i(o.i) { retain(); }
  Value(Value&& o) noexcept : t(o.t), isChar(o.isChar), i(o.i) { o.t = V_VOID; o.i = 0; }
  Value& operator=(const Value& o) {
    if (this != &o) {
      o.retain();
      release();
      t = o.t; isChar = o.isChar; i = o.i;
    }
    return *this;
  }
  Value& operator=(Value&& o) noexcept {
    if (this != &o) {
      release();
      t = o.t; isChar = o.isChar; i = o.i;
      o.t = V_VOID; o.i = 0;
    }
    return *this;
  }
  ~Value() { release(); }
  inline void retain() const;
  inline void release();

  static Value I(int64_t v, bool ch = false) { Value x; x.t = V_INT; x.isChar = ch ? 1 : 0; x.i = v; return x; }
  static Value F(double v) { Value x; x.t = V_FLT; x.f = v; return x; }
  static Value S(const std::string& s);
  static Value S(std::string&& s);
  static Value Ref(Value* p) { Value x; x.t = V_REF; x.r = p; return x; }
  static Value A(ArrObj* a) { Value x; x.t = V_ARR; x.a = a; return x; }
  static Value Fn(int idx) { Value x; x.t = V_FUNC; x.i = idx; return x; }
  bool isNum() const { return t == V_INT || t == V_FLT; }
};

struct StrObj {
  int32_t rc;
  std::string s;
};

struct ArrObj {
  int32_t rc;
  uint8_t elemTy;
  int16_t sid = -1;   // >= 0: es una estructura (los campos van en v)
  std::vector<Value> v;
};

inline void Value::retain() const {
  if (t == V_STR) s->rc++;
  else if (t == V_ARR) a->rc++;
}
inline void Value::release() {
  if (t == V_STR) { if (--s->rc == 0) delete s; }
  else if (t == V_ARR) { if (--a->rc == 0) delete a; }
  t = V_VOID;
  i = 0;
}
Value Value::S(const std::string& str) {
  Value x; x.t = V_STR; x.s = new StrObj; x.s->rc = 1; x.s->s = str; return x;
}
Value Value::S(std::string&& str) {
  Value x; x.t = V_STR; x.s = new StrObj; x.s->rc = 1; x.s->s = std::move(str); return x;
}

// ---------------------------------------------------------------------
//  Arbol sintactico
// ---------------------------------------------------------------------
enum NodeKind : uint8_t {
  // expresiones
  N_NUM, N_FLT, N_STR, N_LVAR, N_GVAR, N_ASSIGN, N_BIN, N_AND, N_OR, N_NOT, N_NEG, N_POS,
  N_BITNOT, N_PREINC, N_PREDEC, N_POSTINC, N_POSTDEC, N_TERN, N_CALL, N_PCALL, N_BUILTIN,
  N_METHOD, N_INDEX, N_CAST, N_SIZEOF, N_COMMA, N_INITLIST,
  // sentencias
  N_BLOCK, N_EXPR, N_DECL, N_IF, N_WHILE, N_DOWHILE, N_FOR, N_FOREACH, N_BREAK, N_CONTINUE,
  N_RETURN, N_SWITCH, N_CASE, N_DEFAULT, N_EMPTY, N_DIM,
  // referencia a funcion (para interrupciones) y &variable
  N_FUNCREF, N_ADDR,
  // estructuras: x.campo y .campo = valor en { }
  N_FIELD, N_DESIG
};

struct Node {
  uint8_t kind;
  uint8_t op;
  uint16_t line;
  int32_t idx;
  union {
    int64_t ival;
    double fval;
  };
  Node* a;
  Node* b;
  Node* c;
  Node* d;
  Node* next;
};

// flags para N_DECL / N_FOREACH (en ival)
static const int DF_GLOBAL = 1;
static const int DF_REF = 2;

struct Arena {
  static const size_t CHUNK = 4096;
  std::vector<char*> chunks;
  size_t used;
  size_t total;
  Arena() : used(CHUNK), total(0) {}
  ~Arena() { clear(); }
  void* alloc(size_t n) {
    n = (n + 7) & ~(size_t)7;
    if (used + n > CHUNK) {
      char* c = (char*)malloc(CHUNK);
      if (!c) return nullptr;
      chunks.push_back(c);
      used = 0;
      total += CHUNK;
    }
    void* p = chunks.back() + used;
    used += n;
    return p;
  }
  void clear() {
    for (size_t k = 0; k < chunks.size(); k++) free(chunks[k]);
    chunks.clear();
    used = CHUNK;
    total = 0;
  }
};

struct Param {
  uint8_t ty;
  bool isRef;
  Node* def;
};

struct Func {
  std::string name;
  uint8_t retTy;
  std::vector<Param> params;
  int nslots;
  Node* body;
  int line;
  bool defined;
};

// ---------------------------------------------------------------------
//  Funciones integradas
// ---------------------------------------------------------------------
enum BuiltinId {
  B_PINMODE, B_DWRITE, B_DREAD, B_AREAD, B_AWRITE, B_AREADRES, B_DACWRITE, B_TOUCHREAD,
  B_TONE, B_NOTONE, B_PULSEIN, B_SHIFTOUT, B_SHIFTIN, B_DELAY, B_DELAYUS, B_MILLIS, B_MICROS,
  B_RANDOM, B_RANDOMSEED, B_MAP, B_CONSTRAIN, B_MIN, B_MAX, B_ABS, B_SQ, B_POW, B_SQRT,
  B_SIN, B_COS, B_TAN, B_ASIN, B_ACOS, B_ATAN, B_ATAN2, B_EXP, B_LOG, B_LOG10, B_FLOOR,
  B_CEIL, B_ROUND, B_FABS, B_FMOD, B_RADIANS, B_DEGREES, B_ISNAN, B_ISINF, B_BITREAD,
  B_BITWRITE, B_BITSET, B_BITCLEAR, B_BIT, B_LOWBYTE, B_HIGHBYTE, B_ISDIGIT, B_ISALPHA,
  B_ISALNUM, B_ISSPACE, B_ISUPPER, B_ISLOWER, B_TOUPPER, B_TOLOWER, B_F, B_YIELD,
  B_S_BEGIN, B_S_PRINT, B_S_PRINTLN, B_S_PRINTF, B_S_WRITE, B_S_AVAILABLE, B_S_READ,
  B_S_PEEK, B_S_READSTRING, B_S_READSTRINGUNTIL, B_S_PARSEINT, B_S_PARSEFLOAT, B_S_FLUSH,
  B_S_SETTIMEOUT, B_STRING2, B_STRLEN, B__COUNT
};

struct BuiltinDef {
  const char* name;
  int8_t minA;
  int8_t maxA;
};

static const BuiltinDef kBuiltins[B__COUNT] = {
  {"pinMode", 2, 2}, {"digitalWrite", 2, 2}, {"digitalRead", 1, 1}, {"analogRead", 1, 1},
  {"analogWrite", 2, 2}, {"analogReadResolution", 1, 1}, {"dacWrite", 2, 2}, {"touchRead", 1, 1},
  {"tone", 2, 3}, {"noTone", 1, 1}, {"pulseIn", 2, 3}, {"shiftOut", 4, 4}, {"shiftIn", 3, 3},
  {"delay", 1, 1}, {"delayMicroseconds", 1, 1}, {"millis", 0, 0}, {"micros", 0, 0},
  {"random", 1, 2}, {"randomSeed", 1, 1}, {"map", 5, 5}, {"constrain", 3, 3}, {"min", 2, 2},
  {"max", 2, 2}, {"abs", 1, 1}, {"sq", 1, 1}, {"pow", 2, 2}, {"sqrt", 1, 1},
  {"sin", 1, 1}, {"cos", 1, 1}, {"tan", 1, 1}, {"asin", 1, 1}, {"acos", 1, 1}, {"atan", 1, 1},
  {"atan2", 2, 2}, {"exp", 1, 1}, {"log", 1, 1}, {"log10", 1, 1}, {"floor", 1, 1},
  {"ceil", 1, 1}, {"round", 1, 1}, {"fabs", 1, 1}, {"fmod", 2, 2}, {"radians", 1, 1},
  {"degrees", 1, 1}, {"isnan", 1, 1}, {"isinf", 1, 1}, {"bitRead", 2, 2},
  {"bitWrite", 3, 3}, {"bitSet", 2, 2}, {"bitClear", 2, 2}, {"bit", 1, 1}, {"lowByte", 1, 1},
  {"highByte", 1, 1}, {"isDigit", 1, 1}, {"isAlpha", 1, 1}, {"isAlphaNumeric", 1, 1},
  {"isSpace", 1, 1}, {"isUpperCase", 1, 1}, {"isLowerCase", 1, 1}, {"toupper", 1, 1},
  {"tolower", 1, 1}, {"F", 1, 1}, {"yield", 0, 0},
  {"Serial.begin", 0, 2}, {"Serial.print", 1, 2}, {"Serial.println", 0, 2},
  {"Serial.printf", 1, MAX_ARGS}, {"Serial.write", 1, 1}, {"Serial.available", 0, 0},
  {"Serial.read", 0, 0}, {"Serial.peek", 0, 0}, {"Serial.readString", 0, 0},
  {"Serial.readStringUntil", 1, 1}, {"Serial.parseInt", 0, 0}, {"Serial.parseFloat", 0, 0},
  {"Serial.flush", 0, 0}, {"Serial.setTimeout", 1, 1}, {"String", 2, 2}, {"strlen", 1, 1},
};

// ---------------------------------------------------------------------
//  Funciones del ESP32 (las ejecuta el firmware con Hal::native) y
//  funciones especiales que resuelve el propio interprete (codigo < 0)
// ---------------------------------------------------------------------
enum NativeFlags { XF_SELF = 1, XF_BUS = 2, XF_NOEVAL = 4, XF_STRUCT = 8 };
// estructuras de ESP-IDF que ya trae el interprete (en este orden)
enum BuiltinStruct { BS_LEDC_FLAGS = 0, BS_LEDC_TIMER, BS_LEDC_CHANNEL, BS_GPIO_CONFIG, BS__COUNT };
enum XSpecial {
  XS_ATTACH_INT = -1, XS_DETACH_INT = -2, XS_TIMER_BEGIN = -3, XS_TIMER_ATTACH = -4,
  XS_TIMER_DETACH = -5, XS_NOINT = -6, XS_INT = -7, XS_CRIT_ENTER = -8, XS_CRIT_EXIT = -9,
  XS_PIN2INT = -10, XS_FREQ_READ = -11, XS_SPISETTINGS = -12, XS_WIRE_WRITE = -13,
  XS_SPI_TRANSFER = -14, XS_STR = -15, XS_FMIN = -16, XS_FMAX = -17, XS_HYPOT = -18,
  XS_TRUNC = -19, XS_CBRT = -20, XS_VTASKDELAY = -21, XS_IDENT = -22, XS_TICKS = -23,
  XS_DELAYUS = -24, XS_ESPCHECK = -25, XS_ESPCHECK_NA = -26, XS_ERRNAME = -27, XS_PRINTF = -28,
  XS_LOG = -29, XS_MEMSET = -30, XS_NOOP = -31, XS_BIT = -32
};

struct NativeDef {
  const char* name;
  int8_t minA, maxA;  // en los metodos de Servo incluye el propio objeto
  int16_t nf;         // NativeFn (> 0) o XSpecial (< 0)
  char ret;           // 'v' nada, 'i' entero, 'b' bool, 'f' decimal, 's' texto
  int8_t pinArg;      // argumento que es un pin (-1 ninguno), sin contar el objeto
  bool pinOut;
  uint8_t flags;
  int16_t aux;
};

static const NativeDef kNatives[] = {
  // PWM / LEDC (nombres del core 3.x y del 2.x)
  {"ledcAttach", 3, 3, NF_LEDC_ATTACH, 'b', 0, true, 0, 0},
  {"ledcAttachChannel", 4, 4, NF_LEDC_ATTACH_CH, 'b', 0, true, 0, 0},
  {"ledcSetup", 3, 3, NF_LEDC_SETUP, 'f', -1, false, 0, 0},
  {"ledcAttachPin", 2, 2, NF_LEDC_ATTACH_PIN, 'v', 0, true, 0, 0},
  {"ledcDetach", 1, 1, NF_LEDC_DETACH, 'b', 0, true, 0, 0},
  {"ledcDetachPin", 1, 1, NF_LEDC_DETACH, 'b', 0, true, 0, 0},
  {"ledcWrite", 2, 2, NF_LEDC_WRITE, 'b', -1, false, 0, 0},
  {"ledcWriteTone", 2, 2, NF_LEDC_TONE, 'i', -1, false, 0, 0},
  {"ledcWriteNote", 3, 3, NF_LEDC_NOTE, 'i', -1, false, 0, 0},
  {"ledcRead", 1, 1, NF_LEDC_READ, 'i', -1, false, 0, 0},
  {"ledcReadFreq", 1, 1, NF_LEDC_READ_FREQ, 'i', -1, false, 0, 0},
  {"ledcChangeFrequency", 3, 3, NF_LEDC_CHANGE_FREQ, 'i', -1, false, 0, 0},
  {"ledcFade", 4, 4, NF_LEDC_FADE, 'b', 0, true, 0, 0},
  {"analogWriteFrequency", 1, 2, NF_AW_FREQ, 'v', -1, false, 0, 0},
  {"analogWriteResolution", 1, 2, NF_AW_RES, 'v', -1, false, 0, 0},
  // generadores de senal por hardware (X-RTbox)
  {"signalSine", 2, 4, NF_SIG_SINE, 'f', 0, true, 0, 0},
  {"signalTriangle", 2, 4, NF_SIG_TRIANGLE, 'f', 0, true, 0, 0},
  {"signalSawtooth", 2, 4, NF_SIG_SAW, 'f', 0, true, 0, 0},
  {"signalSquare", 2, 3, NF_SIG_SQUARE, 'f', 0, true, 0, 0},
  {"signalStop", 1, 1, NF_SIG_STOP, 'v', 0, true, 0, 0},
  {"senoidal", 2, 4, NF_SIG_SINE, 'f', 0, true, 0, 0},
  {"triangular", 2, 4, NF_SIG_TRIANGLE, 'f', 0, true, 0, 0},
  {"dienteDeSierra", 2, 4, NF_SIG_SAW, 'f', 0, true, 0, 0},
  {"cuadrada", 2, 3, NF_SIG_SQUARE, 'f', 0, true, 0, 0},
  {"pararSenal", 1, 1, NF_SIG_STOP, 'v', 0, true, 0, 0},
  {"dacDisable", 1, 1, NF_DAC_DISABLE, 'v', 0, true, 0, 0},
  // medida
  {"pulseCounterStart", 1, 2, NF_CNT_START, 'b', 0, false, 0, 0},
  {"pulseCounterRead", 1, 1, NF_CNT_READ, 'i', 0, false, 0, 0},
  {"pulseCounterReset", 1, 1, NF_CNT_RESET, 'v', 0, false, 0, 0},
  {"pulseCounterStop", 1, 1, NF_CNT_STOP, 'v', 0, false, 0, 0},
  {"frequencyRead", 1, 2, XS_FREQ_READ, 'f', 0, false, 0, 0},
  {"medirFrecuencia", 1, 2, XS_FREQ_READ, 'f', 0, false, 0, 0},
  {"pulseInLong", 2, 3, NF_PULSEIN_LONG, 'i', 0, false, 0, 0},
  // interrupciones
  {"attachInterrupt", 3, 3, XS_ATTACH_INT, 'v', 0, false, 0, 0},
  {"detachInterrupt", 1, 1, XS_DETACH_INT, 'v', 0, false, 0, 0},
  {"digitalPinToInterrupt", 1, 1, XS_PIN2INT, 'i', -1, false, 0, 0},
  {"noInterrupts", 0, 0, XS_NOINT, 'v', -1, false, XF_NOEVAL, 0},
  {"interrupts", 0, 0, XS_INT, 'v', -1, false, XF_NOEVAL, 0},
  {"portENTER_CRITICAL", 1, 1, XS_CRIT_ENTER, 'v', -1, false, XF_NOEVAL, 0},
  {"portEXIT_CRITICAL", 1, 1, XS_CRIT_EXIT, 'v', -1, false, XF_NOEVAL, 0},
  {"portENTER_CRITICAL_ISR", 1, 1, XS_CRIT_ENTER, 'v', -1, false, XF_NOEVAL, 0},
  {"portEXIT_CRITICAL_ISR", 1, 1, XS_CRIT_EXIT, 'v', -1, false, XF_NOEVAL, 0},
  // temporizadores (estilo core 3.x y 2.x)
  {"timerBegin", 1, 3, XS_TIMER_BEGIN, 'i', -1, false, 0, 0},
  {"timerAttachInterrupt", 2, 3, XS_TIMER_ATTACH, 'v', -1, false, 0, 0},
  {"timerDetachInterrupt", 1, 1, XS_TIMER_DETACH, 'v', -1, false, 0, 0},
  {"timerAlarm", 2, 4, NF_TIMER_ALARM, 'v', -1, false, 0, 0},
  {"timerAlarmWrite", 2, 3, NF_TIMER_ALARM_WRITE, 'v', -1, false, 0, 0},
  {"timerAlarmEnable", 1, 1, NF_TIMER_ALARM_ENABLE, 'v', -1, false, 0, 0},
  {"timerAlarmDisable", 1, 1, NF_TIMER_ALARM_DISABLE, 'v', -1, false, 0, 0},
  {"timerStart", 1, 1, NF_TIMER_START, 'v', -1, false, 0, 0},
  {"timerStop", 1, 1, NF_TIMER_STOP, 'v', -1, false, 0, 0},
  {"timerRestart", 1, 1, NF_TIMER_RESTART, 'v', -1, false, 0, 0},
  {"timerEnd", 1, 1, NF_TIMER_END, 'v', -1, false, 0, 0},
  {"timerRead", 1, 1, NF_TIMER_READ, 'i', -1, false, 0, 0},
  {"timerReadMicros", 1, 1, NF_TIMER_READ_US, 'i', -1, false, 0, 0},
  {"timerReadMillis", 1, 1, NF_TIMER_READ_MS, 'i', -1, false, 0, 0},
  {"timerReadSeconds", 1, 1, NF_TIMER_READ_S, 'f', -1, false, 0, 0},
  {"timerWrite", 2, 2, NF_TIMER_WRITE, 'v', -1, false, 0, 0},
  // I2C: Wire y Wire1
  {"Wire.begin", 0, 3, NF_WIRE_BEGIN, 'b', -1, false, XF_BUS, 0},
  {"Wire.end", 0, 0, NF_WIRE_END, 'b', -1, false, XF_BUS, 0},
  {"Wire.setClock", 1, 1, NF_WIRE_CLOCK, 'v', -1, false, XF_BUS, 0},
  {"Wire.getClock", 0, 0, NF_WIRE_GETCLOCK, 'i', -1, false, XF_BUS, 0},
  {"Wire.beginTransmission", 1, 1, NF_WIRE_BEGINTX, 'v', -1, false, XF_BUS, 0},
  {"Wire.write", 1, 2, XS_WIRE_WRITE, 'i', -1, false, XF_BUS, 0},
  {"Wire.endTransmission", 0, 1, NF_WIRE_ENDTX, 'i', -1, false, XF_BUS, 0},
  {"Wire.requestFrom", 2, 3, NF_WIRE_REQUEST, 'i', -1, false, XF_BUS, 0},
  {"Wire.available", 0, 0, NF_WIRE_AVAILABLE, 'i', -1, false, XF_BUS, 0},
  {"Wire.read", 0, 0, NF_WIRE_READ, 'i', -1, false, XF_BUS, 0},
  {"Wire.peek", 0, 0, NF_WIRE_PEEK, 'i', -1, false, XF_BUS, 0},
  // SPI
  {"SPI.begin", 0, 4, NF_SPI_BEGIN, 'v', -1, false, 0, 0},
  {"SPI.end", 0, 0, NF_SPI_END, 'v', -1, false, 0, 0},
  {"SPI.beginTransaction", 1, 1, NF_SPI_BEGINTX, 'v', -1, false, 0, 0},
  {"SPI.endTransaction", 0, 0, NF_SPI_ENDTX, 'v', -1, false, 0, 0},
  {"SPI.transfer", 1, 2, XS_SPI_TRANSFER, 'i', -1, false, 0, 0},
  {"SPI.transfer16", 1, 1, NF_SPI_TRANSFER16, 'i', -1, false, 0, 0},
  {"SPI.transfer32", 1, 1, NF_SPI_TRANSFER32, 'i', -1, false, 0, 0},
  {"SPI.setFrequency", 1, 1, NF_SPI_FREQ, 'v', -1, false, 0, 0},
  {"SPI.setDataMode", 1, 1, NF_SPI_MODE, 'v', -1, false, 0, 0},
  {"SPI.setBitOrder", 1, 1, NF_SPI_ORDER, 'v', -1, false, 0, 0},
  {"SPI.write", 1, 1, NF_SPI_WRITE, 'v', -1, false, 0, 0},
  {"SPI.write16", 1, 1, NF_SPI_WRITE16, 'v', -1, false, 0, 0},
  {"SPI.write32", 1, 1, NF_SPI_WRITE32, 'v', -1, false, 0, 0},
  {"SPISettings", 0, 3, XS_SPISETTINGS, 'i', -1, false, 0, 0},
  // Servo (metodos sobre una variable Servo)
  {"Servo.attach", 2, 4, NF_SERVO_ATTACH, 'i', 0, true, XF_SELF, 0},
  {"Servo.write", 2, 2, NF_SERVO_WRITE, 'v', -1, false, XF_SELF, 0},
  {"Servo.writeMicroseconds", 2, 2, NF_SERVO_WRITE_US, 'v', -1, false, XF_SELF, 0},
  {"Servo.read", 1, 1, NF_SERVO_READ, 'i', -1, false, XF_SELF, 0},
  {"Servo.readMicroseconds", 1, 1, NF_SERVO_READ_US, 'i', -1, false, XF_SELF, 0},
  {"Servo.attached", 1, 1, NF_SERVO_ATTACHED, 'b', -1, false, XF_SELF, 0},
  {"Servo.detach", 1, 1, NF_SERVO_DETACH, 'v', -1, false, XF_SELF, 0},
  // sistema
  {"ESP.restart", 0, 0, NF_ESP_RESTART, 'v', -1, false, 0, 0},
  {"ESP.getFreeHeap", 0, 0, NF_ESP_FREEHEAP, 'i', -1, false, 0, 0},
  {"ESP.getHeapSize", 0, 0, NF_ESP_HEAPSIZE, 'i', -1, false, 0, 0},
  {"ESP.getCpuFreqMHz", 0, 0, NF_ESP_CPUFREQ, 'i', -1, false, 0, 0},
  {"ESP.getChipCores", 0, 0, NF_ESP_CORES, 'i', -1, false, 0, 0},
  {"ESP.getFlashChipSize", 0, 0, NF_ESP_FLASHSIZE, 'i', -1, false, 0, 0},
  {"ESP.getEfuseMac", 0, 0, NF_ESP_MAC, 'i', -1, false, 0, 0},
  {"ESP.getChipModel", 0, 0, XS_STR, 's', -1, false, 0, NS_CHIP_MODEL},
  {"ESP.getSdkVersion", 0, 0, XS_STR, 's', -1, false, 0, NS_SDK_VERSION},
  {"temperatureRead", 0, 0, NF_TEMPERATURE, 'f', -1, false, 0, 0},
  {"analogReadMilliVolts", 1, 1, NF_AREAD_MV, 'i', 0, false, 0, 0},
  {"analogSetAttenuation", 1, 1, NF_ASET_ATTEN, 'v', -1, false, 0, 0},
  {"analogSetPinAttenuation", 2, 2, NF_ASET_PIN_ATTEN, 'v', 0, false, 0, 0},
  {"rgbLedWrite", 4, 4, NF_RGBLED, 'v', -1, false, 0, 0},
  {"neopixelWrite", 4, 4, NF_RGBLED, 'v', -1, false, 0, 0},
  {"setCpuFrequencyMhz", 1, 1, NF_SET_CPUFREQ, 'b', -1, false, 0, 0},
  {"getCpuFrequencyMhz", 0, 0, NF_ESP_CPUFREQ, 'i', -1, false, 0, 0},
  {"esp_random", 0, 0, NF_ESP_RANDOM, 'i', -1, false, 0, 0},
  {"esp_timer_get_time", 0, 0, NF_ESP_TIMER_US, 'i', -1, false, 0, 0},
  {"fmin", 2, 2, XS_FMIN, 'f', -1, false, 0, 0},
  {"fmax", 2, 2, XS_FMAX, 'f', -1, false, 0, 0},
  {"hypot", 2, 2, XS_HYPOT, 'f', -1, false, 0, 0},
  {"trunc", 1, 1, XS_TRUNC, 'f', -1, false, 0, 0},
  {"cbrt", 1, 1, XS_CBRT, 'f', -1, false, 0, 0},
  {"ledcOutputInvert", 2, 2, NF_LEDC_INVERT, 'b', 0, true, 0, 0},
  {"sigmaDeltaAttach", 2, 2, NF_SD_ATTACH, 'b', 0, true, 0, 0},
  {"sigmaDeltaWrite", 2, 2, NF_SD_WRITE, 'b', 0, true, 0, 0},
  {"sigmaDeltaDetach", 1, 1, NF_SD_DETACH, 'b', 0, true, 0, 0},
  // ESP-IDF: driver/ledc.h
  {"ledc_timer_config", 1, 1, NF_IDF_LEDC_TIMER, 'i', -1, false, XF_STRUCT, BS_LEDC_TIMER},
  {"ledc_channel_config", 1, 1, NF_IDF_LEDC_CHANNEL, 'i', -1, false, XF_STRUCT, BS_LEDC_CHANNEL},
  {"ledc_set_duty", 3, 3, NF_IDF_SET_DUTY, 'i', -1, false, 0, 0},
  {"ledc_update_duty", 2, 2, NF_IDF_UPDATE_DUTY, 'i', -1, false, 0, 0},
  {"ledc_set_duty_and_update", 4, 4, NF_IDF_SET_DUTY_UPDATE, 'i', -1, false, 0, 0},
  {"ledc_set_duty_with_hpoint", 4, 4, NF_IDF_SET_DUTY_HPOINT, 'i', -1, false, 0, 0},
  {"ledc_get_duty", 2, 2, NF_IDF_GET_DUTY, 'i', -1, false, 0, 0},
  {"ledc_get_hpoint", 2, 2, NF_IDF_GET_HPOINT, 'i', -1, false, 0, 0},
  {"ledc_set_freq", 3, 3, NF_IDF_SET_FREQ, 'i', -1, false, 0, 0},
  {"ledc_get_freq", 2, 2, NF_IDF_GET_FREQ, 'i', -1, false, 0, 0},
  {"ledc_stop", 3, 3, NF_IDF_STOP, 'i', -1, false, 0, 0},
  {"ledc_timer_pause", 2, 2, NF_IDF_TIMER_PAUSE, 'i', -1, false, 0, 0},
  {"ledc_timer_resume", 2, 2, NF_IDF_TIMER_RESUME, 'i', -1, false, 0, 0},
  {"ledc_timer_rst", 2, 2, NF_IDF_TIMER_RST, 'i', -1, false, 0, 0},
  {"ledc_bind_channel_timer", 3, 3, NF_IDF_BIND, 'i', -1, false, 0, 0},
  {"ledc_set_pin", 3, 3, NF_IDF_SET_PIN, 'i', 0, true, 0, 0},
  {"ledc_fade_func_install", 1, 1, NF_IDF_FADE_INSTALL, 'i', -1, false, 0, 0},
  {"ledc_fade_func_uninstall", 0, 0, NF_IDF_FADE_UNINSTALL, 'v', -1, false, 0, 0},
  {"ledc_set_fade_with_time", 4, 4, NF_IDF_FADE_TIME, 'i', -1, false, 0, 0},
  {"ledc_set_fade_with_step", 5, 5, NF_IDF_FADE_STEP, 'i', -1, false, 0, 0},
  {"ledc_fade_start", 3, 3, NF_IDF_FADE_START, 'i', -1, false, 0, 0},
  {"ledc_fade_stop", 2, 2, NF_IDF_FADE_STOP, 'i', -1, false, 0, 0},
  {"ledc_set_fade_time_and_start", 5, 5, NF_IDF_FADE_TIME_START, 'i', -1, false, 0, 0},
  {"ledc_set_fade_step_and_start", 6, 6, NF_IDF_FADE_STEP_START, 'i', -1, false, 0, 0},
  {"ledc_find_suitable_duty_resolution", 2, 2, NF_IDF_FIND_RES, 'i', -1, false, 0, 0},
  // ESP-IDF: driver/gpio.h
  {"gpio_config", 1, 1, NF_IDF_GPIO_CONFIG, 'i', -1, false, XF_STRUCT, BS_GPIO_CONFIG},
  {"gpio_set_level", 2, 2, NF_IDF_GPIO_SET_LEVEL, 'i', 0, true, 0, 0},
  {"gpio_get_level", 1, 1, NF_IDF_GPIO_GET_LEVEL, 'i', 0, false, 0, 0},
  {"gpio_set_direction", 2, 2, NF_IDF_GPIO_SET_DIR, 'i', 0, false, 0, 0},
  {"gpio_reset_pin", 1, 1, NF_IDF_GPIO_RESET, 'i', 0, false, 0, 0},
  {"gpio_set_pull_mode", 2, 2, NF_IDF_GPIO_PULL, 'i', 0, false, 0, 0},
  {"gpio_pullup_en", 1, 1, NF_IDF_GPIO_PU_EN, 'i', 0, false, 0, 0},
  {"gpio_pullup_dis", 1, 1, NF_IDF_GPIO_PU_DIS, 'i', 0, false, 0, 0},
  {"gpio_pulldown_en", 1, 1, NF_IDF_GPIO_PD_EN, 'i', 0, false, 0, 0},
  {"gpio_pulldown_dis", 1, 1, NF_IDF_GPIO_PD_DIS, 'i', 0, false, 0, 0},
  // ESP-IDF / FreeRTOS: utilidades
  {"ESP_ERROR_CHECK", 1, 1, XS_ESPCHECK, 'v', -1, false, 0, 0},
  {"ESP_ERROR_CHECK_WITHOUT_ABORT", 1, 1, XS_ESPCHECK_NA, 'i', -1, false, 0, 0},
  {"esp_err_to_name", 1, 1, XS_ERRNAME, 's', -1, false, 0, 0},
  {"vTaskDelay", 1, 1, XS_VTASKDELAY, 'v', -1, false, 0, 0},
  {"vTaskDelete", 1, 1, XS_NOOP, 'v', -1, false, 0, 0},
  {"pdMS_TO_TICKS", 1, 1, XS_IDENT, 'i', -1, false, 0, 0},
  {"pdTICKS_TO_MS", 1, 1, XS_IDENT, 'i', -1, false, 0, 0},
  {"xTaskGetTickCount", 0, 0, XS_TICKS, 'i', -1, false, 0, 0},
  {"esp_rom_delay_us", 1, 1, XS_DELAYUS, 'v', -1, false, 0, 0},
  {"ets_delay_us", 1, 1, XS_DELAYUS, 'v', -1, false, 0, 0},
  {"esp_restart", 0, 0, NF_ESP_RESTART, 'v', -1, false, 0, 0},
  {"esp_get_free_heap_size", 0, 0, NF_ESP_FREEHEAP, 'i', -1, false, 0, 0},
  {"printf", 1, MAX_ARGS, XS_PRINTF, 'i', -1, false, 0, 0},
  {"ESP_LOGI", 2, MAX_ARGS, XS_LOG, 'v', -1, false, 0, 'I'},
  {"ESP_LOGW", 2, MAX_ARGS, XS_LOG, 'v', -1, false, 0, 'W'},
  {"ESP_LOGE", 2, MAX_ARGS, XS_LOG, 'v', -1, false, 0, 'E'},
  {"ESP_LOGD", 2, MAX_ARGS, XS_LOG, 'v', -1, false, 0, 'D'},
  {"ESP_LOGV", 2, MAX_ARGS, XS_LOG, 'v', -1, false, 0, 'V'},
  {"memset", 3, 3, XS_MEMSET, 'v', -1, false, 0, 0},
  {"BIT", 1, 1, XS_BIT, 'i', -1, false, 0, 0},
};
static const int kNativeCount = (int)(sizeof(kNatives) / sizeof(kNatives[0]));

static int findBuiltin(const std::string& name) {
  for (int k = 0; k < B__COUNT; k++)
    if (name == kBuiltins[k].name) return k;
  for (int k = 0; k < kNativeCount; k++)
    if (name == kNatives[k].name) return B__COUNT + k;
  return -1;
}

static void builtinRange(int bi, int& lo, int& hi, const char*& name) {
  if (bi < B__COUNT) {
    lo = kBuiltins[bi].minA;
    hi = kBuiltins[bi].maxA;
    name = kBuiltins[bi].name;
  } else {
    const NativeDef& d = kNatives[bi - B__COUNT];
    int self = (d.flags & XF_SELF) ? 1 : 0;
    lo = d.minA;
    hi = d.maxA;
    name = d.name;
    (void)self;
  }
}

enum MethodId {
  M_LENGTH, M_CHARAT, M_SUBSTRING, M_INDEXOF, M_LASTINDEXOF, M_TOINT, M_TOFLOAT, M_TODOUBLE,
  M_EQUALS, M_EQUALSIC, M_STARTSWITH, M_ENDSWITH, M_COMPARETO, M_CSTR, M_ISEMPTY,
  // los siguientes modifican el String
  M_CONCAT, M_TOUPPER, M_TOLOWER, M_TRIM, M_REPLACE, M_REMOVE, M_SETCHARAT, M__COUNT
};
static const char* const kMethods[M__COUNT] = {
  "length", "charAt", "substring", "indexOf", "lastIndexOf", "toInt", "toFloat", "toDouble",
  "equals", "equalsIgnoreCase", "startsWith", "endsWith", "compareTo", "c_str", "isEmpty",
  "concat", "toUpperCase", "toLowerCase", "trim", "replace", "remove", "setCharAt"};
static int findMethod(const std::string& n) {
  for (int k = 0; k < M__COUNT; k++)
    if (n == kMethods[k]) return k;
  return -1;
}

// ---------------------------------------------------------------------
//  Lexer
// ---------------------------------------------------------------------
enum TokType : uint8_t { TK_EOF, TK_IDENT, TK_INT, TK_FLOAT, TK_CHAR, TK_STRING, TK_OP, TK_NL };
enum OpCode : uint16_t {
  OP_EQ = 256, OP_NE, OP_LE, OP_GE, OP_ANDAND, OP_OROR, OP_SHL, OP_SHR, OP_INC, OP_DEC,
  OP_ADDEQ, OP_SUBEQ, OP_MULEQ, OP_DIVEQ, OP_MODEQ, OP_ANDEQ, OP_OREQ, OP_XOREQ, OP_SHLEQ,
  OP_SHREQ, OP_ARROW, OP_SCOPE, OP_ELLIPSIS
};

static const char* opText(int op) {
  static char one[2];
  switch (op) {
    case OP_EQ: return "==";
    case OP_NE: return "!=";
    case OP_LE: return "<=";
    case OP_GE: return ">=";
    case OP_ANDAND: return "&&";
    case OP_OROR: return "||";
    case OP_SHL: return "<<";
    case OP_SHR: return ">>";
    case OP_INC: return "++";
    case OP_DEC: return "--";
    case OP_ADDEQ: return "+=";
    case OP_SUBEQ: return "-=";
    case OP_MULEQ: return "*=";
    case OP_DIVEQ: return "/=";
    case OP_MODEQ: return "%=";
    case OP_ANDEQ: return "&=";
    case OP_OREQ: return "|=";
    case OP_XOREQ: return "^=";
    case OP_SHLEQ: return "<<=";
    case OP_SHREQ: return ">>=";
    case OP_ARROW: return "->";
    case OP_SCOPE: return "::";
    case OP_ELLIPSIS: return "...";
    default: one[0] = (char)op; one[1] = 0; return one;
  }
}

struct Token {
  uint8_t type;
  uint8_t mdepth;
  uint16_t op;
  int line;
  int64_t ival;
  double fval;
  std::string text;
  Token() : type(TK_EOF), mdepth(0), op(0), line(0), ival(0), fval(0) {}
};

struct Macro {
  bool isFunc;
  std::vector<std::string> params;
  std::vector<Token> body;
  Macro() : isFunc(false) {}
};

struct CondState {
  bool active;
  bool taken;
  bool parentActive;
};

class Lexer {
public:
  Lexer(const char* s, size_t n) : errLine(0), src(s), len(n), pos(0), line(1), lineStart(true) {}
  Token next();
  std::string error;
  int errLine;
  int curLine() const { return line; }

private:
  const char* src;
  size_t len;
  size_t pos;
  int line;
  bool lineStart;
  std::map<std::string, Macro> macros;
  std::deque<Token> pending;
  std::vector<CondState> conds;

  void fail(int ln, const char* msg) {
    if (error.empty()) { error = msg; errLine = ln; }
  }
  bool skipping() const { return !conds.empty() && !conds.back().active; }
  Token rawNext() {
    if (!pending.empty()) {
      Token t = pending.front();
      pending.pop_front();
      return t;
    }
    return scan(false);
  }
  Token scan(bool directiveMode);
  void directive();
  void skipRestOfLine();
  bool evalIf(std::vector<Token>& toks);
  int readEscape();
  char peekc(size_t k = 0) const { return pos + k < len ? src[pos + k] : 0; }
};

void Lexer::skipRestOfLine() {
  while (pos < len) {
    char c = src[pos];
    if (c == '\\' && pos + 1 < len && src[pos + 1] == '\n') { pos += 2; line++; continue; }
    if (c == '\n') { pos++; line++; lineStart = true; return; }
    pos++;
  }
}

int Lexer::readEscape() {
  // pos apunta al caracter despues de '\'
  char c = peekc();
  pos++;
  switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': {
      int v = c - '0';
      for (int k = 0; k < 2 && peekc() >= '0' && peekc() <= '7'; k++) { v = v * 8 + (peekc() - '0'); pos++; }
      return v & 0xFF;
    }
    case 'x': {
      int v = 0, cnt = 0;
      while (isxdigit((unsigned char)peekc()) && cnt < 2) {
        char h = peekc();
        v = v * 16 + (isdigit((unsigned char)h) ? h - '0' : (tolower(h) - 'a' + 10));
        pos++; cnt++;
      }
      return v;
    }
    case 'a': return 7;
    case 'b': return 8;
    case 'f': return 12;
    case 'v': return 11;
    case 'e': return 27;
    default: return (unsigned char)c;  // \\ \' \" \?
  }
}

Token Lexer::scan(bool directiveMode) {
  for (;;) {
    // espacios y comentarios
    while (pos < len) {
      char c = src[pos];
      if (c == '\n') {
        if (directiveMode) {
          pos++; line++; lineStart = true;
          Token t; t.type = TK_NL; t.line = line - 1; return t;
        }
        pos++; line++; lineStart = true; continue;
      }
      if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { pos++; continue; }
      if (c == '\\' && pos + 1 < len && (src[pos + 1] == '\n' || (src[pos + 1] == '\r' && pos + 2 < len && src[pos + 2] == '\n'))) {
        pos += (src[pos + 1] == '\n') ? 2 : 3;
        line++;
        continue;
      }
      if (c == '/' && peekc(1) == '/') {
        while (pos < len && src[pos] != '\n') pos++;
        continue;
      }
      if (c == '/' && peekc(1) == '*') {
        int startLine = line;
        pos += 2;
        bool closed = false;
        while (pos < len) {
          if (src[pos] == '*' && peekc(1) == '/') { pos += 2; closed = true; break; }
          if (src[pos] == '\n') line++;
          pos++;
        }
        if (!closed) fail(startLine, "comentario /* sin cerrar");
        continue;
      }
      break;
    }
    Token t;
    t.line = line;
    if (pos >= len) { t.type = directiveMode ? TK_NL : TK_EOF; return t; }
    char c = src[pos];
    if (!directiveMode && c == '#' && lineStart) {
      pos++;
      directive();
      continue;
    }
    if (!directiveMode && skipping()) {
      skipRestOfLine();
      continue;
    }
    lineStart = false;

    // identificadores (se admiten bytes UTF-8 como 'ñ')
    if (isalpha((unsigned char)c) || c == '_' || (unsigned char)c >= 0x80) {
      size_t st = pos;
      while (pos < len && (isalnum((unsigned char)src[pos]) || src[pos] == '_' || (unsigned char)src[pos] >= 0x80)) pos++;
      t.type = TK_IDENT;
      t.text.assign(src + st, pos - st);
      return t;
    }
    // numeros
    if (isdigit((unsigned char)c) || (c == '.' && isdigit((unsigned char)peekc(1)))) {
      size_t st = pos;
      bool isFloat = false;
      uint64_t v = 0;
      if (c == '0' && (peekc(1) == 'x' || peekc(1) == 'X')) {
        pos += 2;
        if (!isxdigit((unsigned char)peekc())) fail(line, "numero hexadecimal mal escrito");
        while (isxdigit((unsigned char)peekc())) {
          char h = peekc();
          v = v * 16 + (isdigit((unsigned char)h) ? h - '0' : (tolower(h) - 'a' + 10));
          pos++;
        }
      } else if (c == '0' && (peekc(1) == 'b' || peekc(1) == 'B') && (peekc(2) == '0' || peekc(2) == '1')) {
        pos += 2;
        while (peekc() == '0' || peekc() == '1') { v = v * 2 + (peekc() - '0'); pos++; }
      } else {
        while (isdigit((unsigned char)peekc())) pos++;
        if (peekc() == '.') { isFloat = true; pos++; while (isdigit((unsigned char)peekc())) pos++; }
        if ((peekc() == 'e' || peekc() == 'E') &&
            (isdigit((unsigned char)peekc(1)) || ((peekc(1) == '+' || peekc(1) == '-') && isdigit((unsigned char)peekc(2))))) {
          isFloat = true;
          pos += 2;
          while (isdigit((unsigned char)peekc())) pos++;
        }
        std::string num(src + st, pos - st);
        if (isFloat) {
          t.fval = strtod(num.c_str(), nullptr);
        } else if (num.size() > 1 && num[0] == '0') {
          for (size_t k = 1; k < num.size(); k++) {
            if (num[k] > '7') { fail(line, "numero octal mal escrito (empieza por 0)"); break; }
            v = v * 8 + (num[k] - '0');
          }
        } else {
          for (size_t k = 0; k < num.size(); k++) v = v * 10 + (num[k] - '0');
        }
      }
      // sufijos u, l, f
      while (peekc() == 'u' || peekc() == 'U' || peekc() == 'l' || peekc() == 'L' || peekc() == 'f' || peekc() == 'F') {
        if ((peekc() == 'f' || peekc() == 'F') && !isFloat) { isFloat = true; t.fval = (double)v; }
        pos++;
      }
      if (isFloat) { t.type = TK_FLOAT; }
      else { t.type = TK_INT; t.ival = (int64_t)v; }
      return t;
    }
    // caracteres
    if (c == '\'') {
      pos++;
      int v = 0;
      if (peekc() == '\\') { pos++; v = readEscape(); }
      else if (peekc() == '\'' || peekc() == 0) { fail(line, "caracter vacio ''"); }
      else {
        v = (unsigned char)peekc(); pos++;
        // UTF-8 de varios bytes: se toma el primer byte
        while ((unsigned char)peekc() >= 0x80 && (unsigned char)peekc() < 0xC0) pos++;
      }
      if (peekc() != '\'') fail(line, "falta la comilla ' de cierre");
      else pos++;
      t.type = TK_CHAR;
      t.ival = (int8_t)v;
      return t;
    }
    // cadenas
    if (c == '"') {
      pos++;
      t.type = TK_STRING;
      while (pos < len && src[pos] != '"') {
        if (src[pos] == '\n') { fail(line, "texto \"...\" sin cerrar"); break; }
        if (src[pos] == '\\') {
          pos++;
          if (peekc() == '\n') { pos++; line++; continue; }
          t.text.push_back((char)readEscape());
        } else {
          t.text.push_back(src[pos]);
          pos++;
        }
      }
      if (pos < len && src[pos] == '"') pos++;
      else fail(t.line, "texto \"...\" sin cerrar");
      return t;
    }
    // operadores
    t.type = TK_OP;
    char c1 = peekc(1), c2 = peekc(2);
    if (c == '<' && c1 == '<' && c2 == '=') { pos += 3; t.op = OP_SHLEQ; return t; }
    if (c == '>' && c1 == '>' && c2 == '=') { pos += 3; t.op = OP_SHREQ; return t; }
    if (c == '.' && c1 == '.' && c2 == '.') { pos += 3; t.op = OP_ELLIPSIS; return t; }
    int two = 0;
    switch (c) {
      case '=': if (c1 == '=') two = OP_EQ; break;
      case '!': if (c1 == '=') two = OP_NE; break;
      case '<': if (c1 == '=') two = OP_LE; else if (c1 == '<') two = OP_SHL; break;
      case '>': if (c1 == '=') two = OP_GE; else if (c1 == '>') two = OP_SHR; break;
      case '&': if (c1 == '&') two = OP_ANDAND; else if (c1 == '=') two = OP_ANDEQ; break;
      case '|': if (c1 == '|') two = OP_OROR; else if (c1 == '=') two = OP_OREQ; break;
      case '+': if (c1 == '+') two = OP_INC; else if (c1 == '=') two = OP_ADDEQ; break;
      case '-': if (c1 == '-') two = OP_DEC; else if (c1 == '=') two = OP_SUBEQ; else if (c1 == '>') two = OP_ARROW; break;
      case '*': if (c1 == '=') two = OP_MULEQ; break;
      case '/': if (c1 == '=') two = OP_DIVEQ; break;
      case '%': if (c1 == '=') two = OP_MODEQ; break;
      case '^': if (c1 == '=') two = OP_XOREQ; break;
      case ':': if (c1 == ':') two = OP_SCOPE; break;
      default: break;
    }
    if (two) { pos += 2; t.op = (uint16_t)two; return t; }
    if (strchr("+-*/%=<>!&|^~?:;,.()[]{}#", c)) { pos++; t.op = (uint16_t)(unsigned char)c; return t; }
    char msg[48];
    if ((unsigned char)c >= 32 && (unsigned char)c < 127) snprintf(msg, sizeof msg, "caracter inesperado '%c'", c);
    else snprintf(msg, sizeof msg, "caracter inesperado (codigo %d)", (unsigned char)c);
    fail(line, msg);
    pos++;
  }
}

bool Lexer::evalIf(std::vector<Token>& tk) {
  // Soporta: #if 0 / #if 1 / #if defined(X) / #if !defined(X) / #if X
  size_t k = 0;
  bool neg = false;
  if (k < tk.size() && tk[k].type == TK_OP && tk[k].op == '!') { neg = true; k++; }
  bool r = true;
  if (k < tk.size()) {
    if (tk[k].type == TK_INT) r = tk[k].ival != 0;
    else if (tk[k].type == TK_IDENT && tk[k].text == "defined") {
      k++;
      if (k < tk.size() && tk[k].type == TK_OP && tk[k].op == '(') k++;
      r = (k < tk.size() && tk[k].type == TK_IDENT) ? macros.count(tk[k].text) > 0 : false;
    } else if (tk[k].type == TK_IDENT) {
      std::map<std::string, Macro>::iterator it = macros.find(tk[k].text);
      if (it == macros.end()) r = false;
      else if (!it->second.body.empty() && it->second.body[0].type == TK_INT) r = it->second.body[0].ival != 0;
      else r = true;
    }
  }
  return neg ? !r : r;
}

void Lexer::directive() {
  int ln = line;
  Token name = scan(true);
  if (name.type == TK_NL) return;  // '#' solo
  if (name.type != TK_IDENT) { skipRestOfLine(); return; }
  const std::string& d = name.text;
  bool parentActive = !skipping();
  if (d == "ifdef" || d == "ifndef") {
    Token id = scan(true);
    bool def = id.type == TK_IDENT && macros.count(id.text) > 0;
    bool act = parentActive && (d == "ifdef" ? def : !def);
    CondState cs = {act, act, parentActive};
    conds.push_back(cs);
    if (id.type != TK_NL) skipRestOfLine();
    return;
  }
  if (d == "if" || d == "elif") {
    std::vector<Token> tk;
    for (;;) { Token x = scan(true); if (x.type == TK_NL) break; tk.push_back(x); }
    if (d == "if") {
      bool act = parentActive && evalIf(tk);
      CondState cs = {act, act, parentActive};
      conds.push_back(cs);
    } else {
      if (conds.empty()) { fail(ln, "#elif sin #if"); return; }
      CondState& cs = conds.back();
      if (cs.taken) cs.active = false;
      else { cs.active = cs.parentActive && evalIf(tk); cs.taken = cs.active; }
    }
    return;
  }
  if (d == "else") {
    if (conds.empty()) { fail(ln, "#else sin #if"); skipRestOfLine(); return; }
    CondState& cs = conds.back();
    cs.active = cs.parentActive && !cs.taken;
    cs.taken = true;
    skipRestOfLine();
    return;
  }
  if (d == "endif") {
    if (conds.empty()) fail(ln, "#endif sin #if");
    else conds.pop_back();
    skipRestOfLine();
    return;
  }
  if (!parentActive) { skipRestOfLine(); return; }
  if (d == "define") {
    // nombre (sin expandir)
    while (pos < len && (src[pos] == ' ' || src[pos] == '\t')) pos++;
    Token id = scan(true);
    if (id.type != TK_IDENT) { fail(ln, "#define necesita un nombre"); if (id.type != TK_NL) skipRestOfLine(); return; }
    Macro m;
    if (peekc() == '(') {  // macro con parametros (sin espacio antes del parentesis)
      pos++;
      m.isFunc = true;
      for (;;) {
        Token p = scan(true);
        if (p.type == TK_OP && p.op == ')') break;
        if (p.type == TK_OP && p.op == ',') continue;
        if (p.type == TK_IDENT) { m.params.push_back(p.text); continue; }
        fail(ln, "parametros de #define mal escritos");
        if (p.type != TK_NL) skipRestOfLine();
        return;
      }
    }
    for (;;) {
      Token b = scan(true);
      if (b.type == TK_NL) break;
      m.body.push_back(b);
    }
    macros[id.text] = m;
    return;
  }
  if (d == "undef") {
    Token id = scan(true);
    if (id.type == TK_IDENT) macros.erase(id.text);
    if (id.type != TK_NL) skipRestOfLine();
    return;
  }
  // #include, #pragma, #error, #warning, #line ... se ignoran
  skipRestOfLine();
}

Token Lexer::next() {
  for (;;) {
    Token t = rawNext();
    if (t.type != TK_IDENT) return t;
    if (t.text == "PROGMEM" || t.text == "IRAM_ATTR" || t.text == "ICACHE_RAM_ATTR" || t.text == "DRAM_ATTR" ||
        t.text == "ARDUINO_ISR_ATTR")
      continue;
    if (t.mdepth >= 16) return t;
    std::map<std::string, Macro>::iterator it = macros.find(t.text);
    if (it == macros.end()) return t;
    const Macro& m = it->second;
    std::vector<Token> out;
    if (!m.isFunc) {
      out = m.body;
    } else {
      Token p = rawNext();
      if (!(p.type == TK_OP && p.op == '(')) {
        pending.push_front(p);
        return t;
      }
      std::vector<std::vector<Token> > args(1);
      int depth = 0;
      for (;;) {
        Token a = rawNext();
        if (a.type == TK_EOF) { fail(t.line, "llamada a macro sin cerrar ')'"); return a; }
        if (a.type == TK_OP) {
          if (a.op == '(') depth++;
          else if (a.op == ')') { if (depth == 0) break; depth--; }
          else if (a.op == ',' && depth == 0) { args.push_back(std::vector<Token>()); continue; }
        }
        args.back().push_back(a);
      }
      if (args.size() == 1 && args[0].empty() && m.params.empty()) args.clear();
      if (args.size() != m.params.size()) {
        fail(t.line, "numero de argumentos incorrecto en la macro");
        return t;
      }
      for (size_t k = 0; k < m.body.size(); k++) {
        const Token& bt = m.body[k];
        bool replaced = false;
        if (bt.type == TK_IDENT) {
          for (size_t q = 0; q < m.params.size(); q++) {
            if (m.params[q] == bt.text) {
              out.insert(out.end(), args[q].begin(), args[q].end());
              replaced = true;
              break;
            }
          }
        }
        if (!replaced) out.push_back(bt);
      }
    }
    for (size_t k = 0; k < out.size(); k++) { out[k].line = t.line; out[k].mdepth = (uint8_t)(t.mdepth + 1); }
    pending.insert(pending.begin(), out.begin(), out.end());
  }
}

// ---------------------------------------------------------------------
//  Implementacion del interprete
// ---------------------------------------------------------------------
enum Flow { F_NORMAL, F_BREAK, F_CONTINUE, F_RETURN, F_ABORT };

struct Impl {
  Hal& hal;
  std::string& err;
  std::atomic<bool>& stopReq;

  // programa
  Arena arena;
  std::vector<Func> funcs;
  std::vector<Value> strConsts;
  std::vector<Node*> globalInits;
  std::vector<uint8_t> globalTys;
  int setupIdx, loopIdx;
  bool loaded;

  // ejecucion
  std::vector<Value> globals;
  std::vector<Value> stack;
  int sp, fp, depth;
  Value retVal;
  Value lrefHold;
  bool abort_;
  uint32_t steps;
  uint32_t rng;
  // estructuras
  struct StructField { int name; uint8_t ty; std::vector<int> dims; Node* def; };
  struct StructDef { std::string name; std::vector<StructField> f; };
  std::vector<StructDef> structs;
  std::vector<std::string> names;
  // interrupciones diferidas: bit del evento -> funcion del programa
  int eventFunc[32];
  int eventMask;
  bool inEvent;

  Impl(Hal& h, std::string& e, std::atomic<bool>& s)
      : hal(h), err(e), stopReq(s), setupIdx(-1), loopIdx(-1), loaded(false), sp(0), fp(0), depth(0),
        abort_(false), steps(0), rng(0x2545F491u), eventMask(0), inEvent(false) {
    for (int k = 0; k < 32; k++) eventFunc[k] = -1;
  }

  void clearProgram() {
    globals.clear();
    stack.clear();
    retVal = Value();
    lrefHold = Value();
    funcs.clear();
    strConsts.clear();
    globalInits.clear();
    globalTys.clear();
    structs.clear();
    names.clear();
    arena.clear();
    setupIdx = loopIdx = -1;
    loaded = false;
  }

  // ---- errores ----
  void fail(int line, const char* fmt, ...) {
    if (abort_) return;
    abort_ = true;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char out[180];
    snprintf(out, sizeof out, "Linea %d: %s", line, buf);
    err = out;
  }
  void stopAbort() {
    if (!abort_) { abort_ = true; err = "Detenido"; }
  }

  // ---- utilidades de valores ----
  uint32_t nextRand() {
    uint32_t x = rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    rng = x ? x : 0x2545F491u;
    return rng;
  }
  static int64_t wrapInt(int64_t x, uint8_t ty) {
    int bits = kTyBits[ty];
    if (ty == TY_BOOL) return x != 0;
    if (bits == 0 || bits >= 64) return x;
    uint64_t m = (((uint64_t)1) << bits) - 1;
    uint64_t u = ((uint64_t)x) & m;
    if (!kTyUns[ty] && (u >> (bits - 1))) return (int64_t)(u | ~m);
    return (int64_t)u;
  }
  static int64_t f2i(double f) {
    if (f != f) return 0;
    if (f >= 9.2e18) return INT64_MAX;
    if (f <= -9.2e18) return INT64_MIN;
    return (int64_t)f;
  }
  static double num(const Value& v) { return v.t == V_FLT ? v.f : (double)v.i; }
  static bool truthy(const Value& v) {
    switch (v.t) {
      case V_INT: return v.i != 0;
      case V_FLT: return v.f != 0.0;
      case V_STR: case V_ARR: case V_FUNC: return true;
      default: return false;
    }
  }
  const char* kindName(const Value& v) {
    switch (v.t) {
      case V_STR: return "un String";
      case V_ARR: return v.a->sid >= 0 ? "una estructura" : "un array";
      case V_VOID: return "un valor vacio (void)";
      case V_FUNC: return "una funcion";
      default: return "un numero";
    }
  }
  Value defaultValue(uint8_t ty) {
    if (tyIsStruct(ty)) return makeStruct(ty - TY_STRUCT0, nullptr, 0);
    if (ty == TY_FLOAT || ty == TY_DOUBLE) return Value::F(0);
    if (ty == TY_STRING) return Value::S(std::string());
    return Value::I(0, ty == TY_CHAR);
  }

  std::string toStr(const Value& v, int fmt = -1) {
    char buf[72];
    switch (v.t) {
      case V_STR: return v.s->s;
      case V_INT: {
        if (v.isChar && fmt < 0) return std::string(1, (char)v.i);
        int base = fmt < 0 ? 10 : fmt;
        if (base == 10 || base < 2 || base > 36) {
          uint64_t u = v.i < 0 ? (uint64_t)0 - (uint64_t)v.i : (uint64_t)v.i;
          char* p = buf + sizeof buf - 1;
          *p = 0;
          do { *--p = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
          if (v.i < 0) *--p = '-';
          return p;
        }
        uint64_t u = (uint64_t)v.i;
        if (v.i < 0 && v.i >= INT32_MIN) u = (uint32_t)v.i;
        char tmp[70];
        int k = 0;
        do { int dgt = (int)(u % base); tmp[k++] = (char)(dgt < 10 ? '0' + dgt : 'A' + dgt - 10); u /= base; } while (u && k < 68);
        std::string s;
        while (k) s.push_back(tmp[--k]);
        return s;
      }
      case V_FLT: {
        int d = fmt < 0 ? 2 : (fmt > 12 ? 12 : fmt);
        double f = v.f;
        if (f != f) return "nan";
        if (isinf(f)) return f > 0 ? "inf" : "-inf";
        if (f > 4294967040.0 || f < -4294967040.0) return "ovf";
        snprintf(buf, sizeof buf, "%.*f", d, f);
        return buf;
      }
      case V_ARR: return v.a->sid >= 0 ? "[estructura]" : "[array]";
      case V_FUNC: return "[funcion]";
      default: return "";
    }
  }

  Value coerce(Value v, uint8_t ty, int line) {
    if (v.t == V_REF) { Value d = *v.r; v = d; }
    if (tyIsStruct(ty)) {   // las estructuras se copian (como en C)
      int sid = ty - TY_STRUCT0;
      if (v.t == V_ARR && v.a->sid == sid) return cloneVal(v);
      fail(line, "se esperaba una estructura '%s' pero es %s", structs[sid].name.c_str(), kindName(v));
      return Value();
    }
    switch (ty) {
      case TY_ANY:
      case TY_VOID:
        return v;
      case TY_STRING:
        if (v.t == V_STR) return v;
        if (v.isNum()) return Value::S(toStr(v));
        fail(line, "no se puede convertir %s a String", kindName(v));
        return Value();
      case TY_FLOAT:
        if (v.t == V_INT) return Value::F((double)(float)v.i);
        if (v.t == V_FLT) return Value::F((double)(float)v.f);
        break;
      case TY_DOUBLE:
        if (v.t == V_INT) return Value::F((double)v.i);
        if (v.t == V_FLT) return v;
        break;
      default:
        if (v.t == V_INT) return Value::I(wrapInt(v.i, ty), ty == TY_CHAR);
        if (v.t == V_FLT) return Value::I(wrapInt(f2i(v.f), ty), ty == TY_CHAR);
        break;
    }
    if (v.t == V_STR) fail(line, "se esperaba un numero pero es un String (usa .toInt() o .toFloat())");
    else fail(line, "se esperaba un numero pero es %s", kindName(v));
    return Value();
  }

  // ---- estructuras ----
  int intern(const std::string& s) {
    for (size_t k = 0; k < names.size(); k++)
      if (names[k] == s) return (int)k;
    names.push_back(s);
    return (int)names.size() - 1;
  }
  int fieldIndex(int sid, int nameId) {
    const StructDef& d = structs[sid];
    for (size_t k = 0; k < d.f.size(); k++)
      if (d.f[k].name == nameId) return (int)k;
    return -1;
  }
  int addStruct(const char* name, const char* const* fields, const uint8_t* tys, int n) {
    StructDef d;
    d.name = name;
    for (int k = 0; k < n; k++) {
      StructField f;
      f.name = intern(fields[k]);
      f.ty = tys[k];
      f.def = nullptr;
      d.f.push_back(f);
    }
    structs.push_back(d);
    return (int)structs.size() - 1;
  }
  void addBuiltinStructs() {
    static const char* const fl[] = {"output_invert"};
    static const uint8_t flt[] = {TY_U32};
    addStruct("ledc_channel_flags", fl, flt, 1);
    static const char* const tm[] = {"speed_mode", "duty_resolution", "timer_num", "freq_hz", "clk_cfg", "deconfigure"};
    static const uint8_t tmt[] = {TY_I32, TY_I32, TY_I32, TY_U32, TY_I32, TY_BOOL};
    addStruct("ledc_timer_config_t", tm, tmt, 6);
    static const char* const ch[] = {"gpio_num", "speed_mode", "channel", "intr_type", "timer_sel", "duty", "hpoint",
                                     "sleep_mode", "flags"};
    static const uint8_t cht[] = {TY_I32, TY_I32, TY_I32, TY_I32, TY_I32, TY_U32, TY_I32, TY_I32,
                                  (uint8_t)(TY_STRUCT0 + BS_LEDC_FLAGS)};
    addStruct("ledc_channel_config_t", ch, cht, 9);
    static const char* const gp[] = {"pin_bit_mask", "mode", "pull_up_en", "pull_down_en", "intr_type"};
    static const uint8_t gpt[] = {TY_U64, TY_I32, TY_I32, TY_I32, TY_I32};
    addStruct("gpio_config_t", gp, gpt, 5);
  }
  Value makeStruct(int sid, Node* init, int line);
  Value fieldValue(const StructField& f, Node* val, int line);
  Value cloneVal(const Value& v);
  int64_t tyBytes(uint8_t ty);
  Value* fieldOf(const Value& base, Node* n, uint8_t& ty);
  Value evalField(Node* n) NOINLINE;
  void fillValue(Value& v, uint8_t ty, int64_t b);
  bool flatten(const Value& v, double* args, int& k, int max, int line);

  // ---- ejecucion ----
  Value eval(Node* n);
  Flow exec(Node* n);
  Flow execLoop(Node* n) NOINLINE;
  Flow execForeach(Node* n) NOINLINE;
  Flow execSwitch(Node* n) NOINLINE;
  Value evalIndex(Node* n) NOINLINE;
  Value evalSizeof(Node* n) NOINLINE;
  Value evalCast(Node* n) NOINLINE;
  Value evalUnary(Node* n) NOINLINE;
  Value* lref(Node* n, uint8_t& ty);
  Value binop(int op, const Value& a, const Value& b, int line);
  Value assign(Node* n) NOINLINE;
  Value incdec(Node* n) NOINLINE;
  Value callUser(int fi, Node* args, int line);
  bool bindArgs(Func& f, Node* args, int newfp, int line) NOINLINE;
  Value finishCall(Func& f, int line) NOINLINE;
  Value builtin(Node* n) NOINLINE;
  Value nativeCall(Node* n) NOINLINE;
  void dispatchEvents(int line) NOINLINE;
  Value method(Node* n) NOINLINE;
  void execDecl(Node* n) NOINLINE;
  Value makeArray(const std::vector<int>& dims, size_t level, uint8_t ty, Node* init, int line);
  int64_t arrBytes(ArrObj* a);
  bool sleepMs(uint32_t ms);
  std::string formatPrintf(const std::string& fmt, Value* av, int n, int line);
  void out(const std::string& s) { if (!s.empty()) hal.write(s.data(), s.size()); }
  bool checkStop() {
    if (stopReq.load(std::memory_order_relaxed)) { stopAbort(); return true; }
    return false;
  }
  RunResult runFunc(int idx);
};

// ---------------------------------------------------------------------
//  Parser
// ---------------------------------------------------------------------
class Parser {
public:
  Parser(Impl& impl, const char* src, size_t len) : P(impl), lex(src, len), failed(false), prevLine(1),
      scopeDepth(0), nextSlot(0), maxSlot(0), inFunc(false), loopDepth(0), switchDepth(0), nest(0) {}
  bool parseProgram();
  std::string error;

private:
  struct LocalVar {
    std::string name;
    int slot;
    uint8_t ty;
    int depth;
    bool isGlobal;
  };
  struct GVar {
    std::string name;
    int slot;
    uint8_t ty;
  };
  struct PendingCall {
    Node* n;
    std::string name;
  };

  Impl& P;
  Lexer lex;
  std::deque<Token> la;
  bool failed;
  int prevLine;
  std::vector<LocalVar> locals;
  int scopeDepth, nextSlot, maxSlot;
  bool inFunc;
  int loopDepth, switchDepth;
  int nest;
  std::vector<GVar> gvars;
  std::map<std::string, int64_t> enumConsts;
  std::map<std::string, uint8_t> typeAliases;
  std::vector<PendingCall> pendingCalls;

  // tokens
  void fill(size_t k) NOINLINE {
    while (la.size() <= k) {
      la.push_back(lex.next());
      if (!lex.error.empty() && !failed) failAt(lex.errLine, lex.error.c_str());
    }
  }
  const Token& peek(size_t k = 0) {
    if (la.size() <= k) fill(k);
    return la[k];
  }
  Token take() NOINLINE {
    peek();
    Token t = std::move(la.front());
    la.pop_front();
    prevLine = t.line;
    return t;
  }
  void skip() NOINLINE {
    peek();
    prevLine = la.front().line;
    la.pop_front();
  }
  int takeLine() { skip(); return prevLine; }
  bool isOp(int op, size_t k = 0) { const Token& t = peek(k); return t.type == TK_OP && t.op == op; }
  bool isIdent(const char* s, size_t k = 0) { const Token& t = peek(k); return t.type == TK_IDENT && t.text == s; }
  bool acceptOp(int op) { if (isOp(op)) { skip(); return true; } return false; }
  std::string desc(const Token& t) {
    switch (t.type) {
      case TK_EOF: return "el final del programa";
      case TK_IDENT: return "'" + t.text + "'";
      case TK_INT: case TK_FLOAT: case TK_CHAR: return "un numero";
      case TK_STRING: return "un texto";
      default: return std::string("'") + opText(t.op) + "'";
    }
  }
  void failAt(int line, const char* fmt, ...) {
    if (failed) return;
    failed = true;
    char buf[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    char o[190];
    snprintf(o, sizeof o, "Linea %d: %s", line, buf);
    error = o;
  }
  bool expectOp(int op) {
    if (acceptOp(op)) return true;
    const Token& t = peek();
    std::string d = desc(t);
    if (op == ';' || op == ')' || op == ']') failAt(prevLine, "falta '%s' (antes de %s)", opText(op), d.c_str());
    else failAt(t.line, "se esperaba '%s' pero hay %s", opText(op), d.c_str());
    return false;
  }
  bool expectIdent(std::string& out) {
    const Token& t = peek();
    if (t.type == TK_IDENT) { out = t.text; skip(); return true; }
    std::string d = desc(t);
    failAt(t.line, "se esperaba un nombre pero hay %s", d.c_str());
    return false;
  }

  // nodos
  Node* mk(uint8_t kind, int line) {
    Node* n = (Node*)P.arena.alloc(sizeof(Node));
    if (!n) { failAt(line, "programa demasiado grande: sin memoria"); return nullptr; }
    memset(n, 0, sizeof(Node));
    n->kind = kind;
    n->line = (uint16_t)(line > 65535 ? 65535 : line);
    return n;
  }

  // tipos
  bool isTypeWord(const std::string& s);
  bool isDeclStart(size_t k = 0);
  int parseType(bool& isStatic, bool& isConst, bool allowRef, bool& isRef);

  // ambitos
  void enterScope() { scopeDepth++; }
  void exitScope() {
    while (!locals.empty() && locals.back().depth == scopeDepth) {
      if (!locals.back().isGlobal) nextSlot--;
      locals.pop_back();
    }
    scopeDepth--;
  }
  int declareLocal(const std::string& name, uint8_t ty, int line) {
    for (size_t k = locals.size(); k-- > 0;) {
      if (locals[k].depth != scopeDepth) break;
      if (locals[k].name == name) { failAt(line, "la variable '%s' ya esta declarada aqui", name.c_str()); return 0; }
    }
    if (nextSlot >= STACK_SLOTS / 2) { failAt(line, "demasiadas variables locales"); return 0; }
    LocalVar lv;
    lv.name = name; lv.slot = nextSlot++; lv.ty = ty; lv.depth = scopeDepth; lv.isGlobal = false;
    locals.push_back(lv);
    if (nextSlot > maxSlot) maxSlot = nextSlot;
    return lv.slot;
  }
  int declareGlobal(const std::string& name, uint8_t ty, int line, bool hidden) {
    if (!hidden) {
      for (size_t k = 0; k < gvars.size(); k++)
        if (gvars[k].name == name) { failAt(line, "la variable global '%s' ya existe", name.c_str()); return 0; }
    }
    int slot = (int)P.globalTys.size();
    P.globalTys.push_back(ty);
    if (!hidden) { GVar g; g.name = name; g.slot = slot; g.ty = ty; gvars.push_back(g); }
    return slot;
  }

  // declaraciones
  bool parseTopLevel();
  bool parseFunction(uint8_t retTy, const std::string& name, int line);
  bool parseEnum(bool global, bool typeOnly = false);
  bool parseTypedef();
  Node* parseStructDef(bool global, bool typeOnly, uint8_t& tyOut);
  void registerIdfTypes();
  Node* parseDeclRest(uint8_t ty, bool isStatic, const std::string& firstName, int firstLine, bool global);
  Node* parseDeclarator(uint8_t ty, bool isStatic, const std::string& name, int line, bool global);
  Node* parseInitList();
  bool foldConst(Node* n, int64_t& out);

  // sentencias
  Node* parseStatement();
  Node* parseBlock();
  Node* parseFor(int line);

  // expresiones
  Node* parseExpr();
  Node* parseAssign();
  Node* parseTernary();
  Node* parseBinary(int minPrec);
  Node* parseUnary();
  Node* parsePostfix(Node* e);
  Node* parsePrimary();
  Node* parseStringLit(int line, const std::string& first) NOINLINE;
  Node* parseCCast(int line) NOINLINE;
  Node* parseParen(int line) NOINLINE;
  Node* parseIdentExpr(const Token& t) NOINLINE;
  Node* parseArgs(int& argc);
  Node* parseIdentifier(const Token& t);
  static int binPrec(const Token& t);
  bool isLvalue(Node* n) { return n && (n->kind == N_LVAR || n->kind == N_GVAR || n->kind == N_INDEX || n->kind == N_FIELD); }
  static uint8_t staticType(Node* n) {
    if (!n) return TY_ANY;
    if (n->kind == N_LVAR || n->kind == N_GVAR) return n->op;
    if (n->kind == N_INDEX) return staticType(n->a);
    return TY_ANY;
  }
  bool resolveCalls();
  const char* pendingName(Node* n) {
    for (size_t k = 0; k < pendingCalls.size(); k++)
      if (pendingCalls[k].n == n) return pendingCalls[k].name.c_str();
    return "?";
  }
  bool undeclared(Node* e, int line) {
    if (e && e->kind == N_FUNCREF) { failAt(line, "'%s' no esta declarado", pendingName(e)); return true; }
    return false;
  }
};

bool Parser::isTypeWord(const std::string& s) {
  static const char* const words[] = {
    "void", "bool", "boolean", "char", "byte", "short", "int", "long", "unsigned", "signed", "float",
    "double", "String", "size_t", "uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t",
    "uint64_t", "int64_t", "word", "auto", "const", "constexpr", "static", "volatile", "inline", "extern",
    "register", "Servo", "SPISettings", "hw_timer_t", "portMUX_TYPE", "esp_timer_handle_t", "struct", nullptr};
  for (int k = 0; words[k]; k++)
    if (s == words[k]) return true;
  return typeAliases.count(s) > 0;
}

bool Parser::isDeclStart(size_t k) {
  const Token& t = peek(k);
  if (t.type != TK_IDENT) return false;
  if (!isTypeWord(t.text)) return false;
  // "int(x)" o "String(x)" al principio son expresiones (conversion)
  if (isOp('(', k + 1)) return false;
  // "Color::ROJO" (enum) es una expresion
  if (isOp(OP_SCOPE, k + 1)) return false;
  return true;
}

int Parser::parseType(bool& isStatic, bool& isConst, bool allowRef, bool& isRef) {
  isStatic = false; isConst = false; isRef = false;
  bool uns = false, sgn = false, hasShort = false;
  int longs = 0;
  int base = -1;  // -1 = ninguno aun
  int line = peek().line;
  int count = 0;
  for (;;) {
    const Token& t = peek();
    if (t.type != TK_IDENT) break;
    const std::string& s = t.text;
    if (s == "const" || s == "constexpr") { isConst = true; skip(); continue; }
    if (s == "static") { isStatic = true; skip(); continue; }
    if (s == "volatile" || s == "inline" || s == "extern" || s == "register") { skip(); continue; }
    if (s == "struct" && base < 0 && count == 0) { skip(); continue; }   // struct Nombre x;
    if (s == "unsigned") { uns = true; skip(); count++; continue; }
    if (s == "signed") { sgn = true; skip(); count++; continue; }
    if (s == "long") { longs++; skip(); count++; continue; }
    if (s == "short") { hasShort = true; skip(); count++; continue; }
    if (base >= 0) break;
    int b = -2;
    if (s == "int") b = -3;  // marcador: int explicito
    else if (s == "void") b = TY_VOID;
    else if (s == "bool" || s == "boolean") b = TY_BOOL;
    else if (s == "char") b = TY_CHAR;
    else if (s == "byte" || s == "uint8_t") b = TY_U8;
    else if (s == "int8_t") b = TY_I8;
    else if (s == "int16_t") b = TY_I16;
    else if (s == "uint16_t" || s == "word") b = TY_U16;
    else if (s == "int32_t") b = TY_I32;
    else if (s == "uint32_t" || s == "size_t") b = TY_U32;
    else if (s == "int64_t") b = TY_I64;
    else if (s == "uint64_t") b = TY_U64;
    else if (s == "float") b = TY_FLOAT;
    else if (s == "double") b = TY_DOUBLE;
    else if (s == "String") b = TY_STRING;
    else if (s == "auto") b = TY_ANY;
    else if (s == "Servo") b = TY_SERVO;
    else if (s == "SPISettings") b = TY_SPISET;
    else if (s == "hw_timer_t" || s == "portMUX_TYPE" || s == "esp_timer_handle_t") b = TY_I32;
    else if (typeAliases.count(s)) b = typeAliases[s];
    if (b == -2) break;
    skip();
    count++;
    if (b == -3) { if (base < 0) base = -3; continue; }
    base = b;
  }
  if (count == 0 && base < 0) {
    std::string d = desc(peek());
    failAt(line, "se esperaba un tipo (int, float, ...) pero hay %s", d.c_str());
    return TY_I32;
  }
  int ty;
  if (base == TY_CHAR) ty = uns ? TY_U8 : (sgn ? TY_I8 : TY_CHAR);
  else if (base == TY_DOUBLE || base == TY_FLOAT) ty = base;
  else if (base >= 0) ty = base;
  else if (hasShort) ty = uns ? TY_U16 : TY_I16;
  else if (longs >= 2) ty = uns ? TY_U64 : TY_I64;
  else ty = uns ? TY_U32 : TY_I32;
  // punteros: char* -> String, otros -> referencia generica (para arrays)
  bool ptr = false;
  while (isOp('*') || isIdent("const")) { if (isOp('*')) ptr = true; skip(); }
  if (ptr) ty = (base == TY_CHAR) ? TY_STRING : TY_ANY;
  if (isOp('&')) {
    if (!allowRef) { failAt(peek().line, "las referencias '&' solo se admiten en parametros de funciones"); return ty; }
    skip();
    isRef = true;
  }
  return ty;
}

void Parser::registerIdfTypes() {
  static const char* const ints[] = {
    "esp_err_t", "ledc_mode_t", "ledc_timer_t", "ledc_channel_t", "ledc_timer_bit_t", "ledc_clk_cfg_t",
    "ledc_intr_type_t", "ledc_fade_mode_t", "ledc_duty_direction_t", "ledc_sleep_mode_t", "ledc_clk_src_t",
    "gpio_num_t", "gpio_mode_t", "gpio_pullup_t", "gpio_pulldown_t", "gpio_int_type_t", "gpio_pull_mode_t",
    "gpio_drive_cap_t", "BaseType_t", "note_t", "adc_attenuation_t", "dac_channel_t", "TaskHandle_t", nullptr};
  for (int k = 0; ints[k]; k++) typeAliases[ints[k]] = TY_I32;
  static const char* const uns[] = {"TickType_t", "UBaseType_t", "uint", nullptr};
  for (int k = 0; uns[k]; k++) typeAliases[uns[k]] = TY_U32;
  P.addBuiltinStructs();
  for (size_t k = 0; k < P.structs.size(); k++) typeAliases[P.structs[k].name] = (uint8_t)(TY_STRUCT0 + k);
}

bool Parser::parseProgram() {
  registerIdfTypes();
  while (!failed) {
    const Token& t = peek();
    if (t.type == TK_EOF) break;
    if (!parseTopLevel()) break;
  }
  if (failed) return false;
  if (!resolveCalls()) return false;
  // setup / loop
  for (size_t k = 0; k < P.funcs.size(); k++) {
    if (P.funcs[k].name == "setup" && P.funcs[k].defined) P.setupIdx = (int)k;
    if (P.funcs[k].name == "loop" && P.funcs[k].defined) P.loopIdx = (int)k;
  }
  if (P.setupIdx < 0 && P.loopIdx < 0) {   // programa de ESP-IDF: app_main() hace de setup()
    for (size_t k = 0; k < P.funcs.size(); k++)
      if (P.funcs[k].name == "app_main" && P.funcs[k].defined) P.setupIdx = (int)k;
  }
  if (P.setupIdx < 0 && P.loopIdx < 0) {
    failAt(1, "el programa necesita una funcion setup() o loop()");
    return false;
  }
  return true;
}

bool Parser::parseTopLevel() {
  const Token& t = peek();
  if (t.type == TK_OP && t.op == ';') { skip(); return true; }
  if (t.type == TK_IDENT) {
    if (t.text == "enum") return parseEnum(true);
    if (t.text == "typedef") return parseTypedef();
    if (t.text == "using") { while (!isOp(';') && peek().type != TK_EOF) skip(); skip(); return true; }
    if (t.text == "extern" && peek(1).type == TK_STRING) {   // extern "C" void app_main(void)
      skip();
      skip();
      if (isOp('{')) { failAt(t.line, "extern \"C\" { ... } no esta soportado; quitalo"); return false; }
      return true;
    }
    if (t.text == "struct" && (isOp('{', 1) || (peek(1).type == TK_IDENT && isOp('{', 2)))) {
      uint8_t sty;
      return parseStructDef(true, false, sty) != nullptr && !failed;
    }
    if (t.text == "struct" && peek(1).type == TK_IDENT && isOp(';', 2)) {   // declaracion adelantada
      skip(); skip(); skip();
      return true;
    }
    if (t.text == "class" || t.text == "union" || t.text == "template" || t.text == "namespace") {
      failAt(t.line, "'%s' no esta soportado por el interprete", t.text.c_str());
      return false;
    }
  }
  if (!isDeclStart()) {
    std::string d = desc(t);
    if (t.type == TK_IDENT && peek(1).type == TK_IDENT)
      failAt(t.line, "tipo desconocido '%s' (solo se admiten tipos basicos y String; las librerias externas no estan disponibles)", t.text.c_str());
    else
      failAt(t.line, "se esperaba una declaracion (variable o funcion) pero hay %s", d.c_str());
    return false;
  }
  bool isStatic, isConst, isRef;
  int ty = parseType(isStatic, isConst, false, isRef);
  if (failed) return false;
  int line = peek().line;
  std::string name;
  if (!expectIdent(name)) return false;
  if (isOp('(')) {
    const Token& n1 = peek(1);
    bool isFunc = (n1.type == TK_OP && n1.op == ')') || (n1.type == TK_IDENT && isTypeWord(n1.text));
    if (isFunc) return parseFunction((uint8_t)ty, name, line);
  }
  Node* decls = parseDeclRest((uint8_t)ty, false, name, line, true);
  return decls != nullptr && !failed;
}

bool Parser::parseFunction(uint8_t retTy, const std::string& name, int line) {
  expectOp('(');
  Func f;
  f.name = name; f.retTy = retTy; f.nslots = 0; f.body = nullptr; f.line = line; f.defined = false;
  std::vector<std::string> pnames;
  locals.clear();
  scopeDepth = 1; nextSlot = 0; maxSlot = 0;
  inFunc = true;
  if (isIdent("void") && isOp(')', 1)) skip();
  while (!failed && !isOp(')')) {
    bool st, cn, isRef;
    int pty = parseType(st, cn, true, isRef);
    if (failed) return false;
    if (cn && isRef) isRef = false;  // const T& se pasa por valor (admite literales)
    std::string pn;
    int pline = peek().line;
    if (peek().type == TK_IDENT) pn = take().text;
    while (isOp('[')) {  // int a[] o int a[10] -> array
      skip();
      while (!isOp(']') && peek().type != TK_EOF) skip();
      expectOp(']');
      pty = TY_ANY;
      isRef = false;
    }
    Param p;
    p.ty = (uint8_t)pty; p.isRef = isRef; p.def = nullptr;
    if (acceptOp('=')) { p.def = parseAssign(); if (failed) return false; }
    if (pty == TY_VOID) { failAt(pline, "un parametro no puede ser void"); return false; }
    f.params.push_back(p);
    pnames.push_back(pn);
    if (!pn.empty()) declareLocal(pn, (uint8_t)pty, pline);
    else {
      nextSlot++;
      if (nextSlot > maxSlot) maxSlot = nextSlot;
    }
    if (!acceptOp(',')) break;
  }
  if (!expectOp(')')) return false;
  if ((int)f.params.size() > MAX_ARGS) { failAt(line, "demasiados parametros en '%s' (max %d)", name.c_str(), MAX_ARGS); return false; }

  // buscar declaracion previa
  int idx = -1;
  for (size_t k = 0; k < P.funcs.size(); k++)
    if (P.funcs[k].name == name) { idx = (int)k; break; }
  if (idx >= 0 && P.funcs[idx].params.size() != f.params.size()) {
    failAt(line, "la funcion '%s' ya existe con otros parametros (la sobrecarga no esta soportada)", name.c_str());
    return false;
  }
  if (idx < 0) { P.funcs.push_back(f); idx = (int)P.funcs.size() - 1; }
  else {
    // conservar valores por defecto del prototipo
    for (size_t k = 0; k < f.params.size(); k++)
      if (!f.params[k].def) f.params[k].def = P.funcs[idx].params[k].def;
    P.funcs[idx].params = f.params;
    P.funcs[idx].retTy = retTy;
  }

  if (acceptOp(';')) {  // prototipo
    inFunc = false;
    locals.clear();
    return true;
  }
  if (!isOp('{')) {
    std::string d = desc(peek());
    failAt(peek().line, "se esperaba '{' para el cuerpo de '%s' pero hay %s", name.c_str(), d.c_str());
    return false;
  }
  if (P.funcs[idx].defined) {
    failAt(line, "la funcion '%s' esta definida dos veces", name.c_str());
    return false;
  }
  loopDepth = 0; switchDepth = 0;
  Node* body = parseBlock();
  if (failed) return false;
  P.funcs[idx].body = body;
  P.funcs[idx].nslots = maxSlot;
  P.funcs[idx].defined = true;
  P.funcs[idx].line = line;
  inFunc = false;
  locals.clear();
  scopeDepth = 0;
  return true;
}

bool Parser::foldConst(Node* n, int64_t& out) {
  if (!n) return false;
  int64_t x, y;
  switch (n->kind) {
    case N_NUM: out = n->ival; return true;
    case N_NEG: if (!foldConst(n->a, x)) return false; out = -x; return true;
    case N_POS: return foldConst(n->a, out);
    case N_BITNOT: if (!foldConst(n->a, x)) return false; out = ~x; return true;
    case N_CAST: return foldConst(n->a, out);
    case N_BIN:
      if (!foldConst(n->a, x) || !foldConst(n->b, y)) return false;
      switch (n->idx) {
        case '+': out = x + y; return true;
        case '-': out = x - y; return true;
        case '*': out = x * y; return true;
        case '/': if (!y) return false; out = x / y; return true;
        case '%': if (!y) return false; out = x % y; return true;
        case '|': out = x | y; return true;
        case '&': out = x & y; return true;
        case '^': out = x ^ y; return true;
        case OP_SHL: out = (y >= 0 && y < 63) ? (x << y) : 0; return true;
        case OP_SHR: out = (y >= 0 && y < 63) ? (x >> y) : 0; return true;
        default: return false;
      }
    default: return false;
  }
}

bool Parser::parseEnum(bool global, bool typeOnly) {
  (void)global;
  int line = takeLine();  // enum
  if (isIdent("class") || isIdent("struct")) skip();
  std::string ename;
  if (peek().type == TK_IDENT) ename = take().text;
  if (acceptOp(':')) { bool a, b, c; parseType(a, b, false, c); }
  if (!ename.empty()) typeAliases[ename] = TY_I32;
  if (!expectOp('{')) return false;
  int64_t val = 0;
  while (!failed && !isOp('}')) {
    std::string nm;
    int ln = peek().line;
    if (!expectIdent(nm)) return false;
    if (acceptOp('=')) {
      Node* e = parseTernary();
      if (failed) return false;
      if (!foldConst(e, val)) { failAt(ln, "el valor de '%s' debe ser una constante", nm.c_str()); return false; }
    }
    enumConsts[nm] = val;
    val++;
    if (!acceptOp(',')) break;
  }
  if (!expectOp('}')) return false;
  if (typeOnly) return true;
  if (peek().type == TK_IDENT) { failAt(line, "declarar variables en la misma linea del enum no esta soportado"); return false; }
  return expectOp(';');
}

// struct Nombre { tipo campo; ... } [variables];
Node* Parser::parseStructDef(bool global, bool typeOnly, uint8_t& tyOut) {
  int line = takeLine();  // struct
  std::string name;
  if (peek().type == TK_IDENT) name = take().text;
  if (!name.empty() && typeAliases.count(name)) { failAt(line, "el tipo '%s' ya existe", name.c_str()); return nullptr; }
  if (!expectOp('{')) return nullptr;
  Impl::StructDef sd;
  sd.name = name.empty() ? "(struct sin nombre)" : name;
  while (!failed && !isOp('}')) {
    if (peek().type == TK_EOF) { failAt(line, "falta '}' para cerrar el struct"); return nullptr; }
    if ((isIdent("public") || isIdent("private") || isIdent("protected")) && isOp(':', 1)) { skip(); skip(); continue; }
    uint8_t fty;
    if (isIdent("struct") && (isOp('{', 1) || (peek(1).type == TK_IDENT && isOp('{', 2)))) {
      if (!parseStructDef(global, true, fty)) return nullptr;   // struct anidado: struct { ... } flags;
    } else {
      bool st, cn, rf;
      fty = (uint8_t)parseType(st, cn, false, rf);
      if (failed) return nullptr;
      if (fty == TY_VOID) { failAt(peek().line, "un campo no puede ser void"); return nullptr; }
    }
    for (;;) {
      std::string fname;
      int fl = peek().line;
      if (!expectIdent(fname)) return nullptr;
      if (isOp('(')) { failAt(fl, "las funciones dentro de un struct no estan soportadas (escribelas fuera)"); return nullptr; }
      Impl::StructField f;
      f.name = P.intern(fname);
      f.ty = fty;
      f.def = nullptr;
      for (size_t k = 0; k < sd.f.size(); k++)
        if (sd.f[k].name == f.name) { failAt(fl, "el campo '%s' esta repetido", fname.c_str()); return nullptr; }
      while (acceptOp('[')) {
        Node* e = parseExpr();
        if (failed) return nullptr;
        int64_t v;
        if (!foldConst(e, v) || v <= 0 || v > MAX_ARRAY_ELEMS) { failAt(fl, "el tamano de '%s[]' debe ser un numero constante", fname.c_str()); return nullptr; }
        f.dims.push_back((int)v);
        if (!expectOp(']')) return nullptr;
      }
      if (acceptOp(':')) { parseTernary(); if (failed) return nullptr; }   // campo de bits: se guarda entero
      if (acceptOp('=')) f.def = isOp('{') ? parseInitList() : parseAssign();
      else if (isOp('{')) f.def = parseInitList();
      if (failed) return nullptr;
      sd.f.push_back(f);
      if (!acceptOp(',')) break;
    }
    if (!expectOp(';')) return nullptr;
  }
  if (!expectOp('}')) return nullptr;
  if (sd.f.empty()) { failAt(line, "el struct no tiene campos"); return nullptr; }
  if (P.structs.size() >= 200) { failAt(line, "demasiados tipos struct"); return nullptr; }
  P.structs.push_back(sd);
  uint8_t ty = (uint8_t)(TY_STRUCT0 + P.structs.size() - 1);
  if (!name.empty()) typeAliases[name] = ty;
  tyOut = ty;
  Node* none = mk(N_EMPTY, line);
  if (typeOnly) return none;
  if (peek().type == TK_IDENT) {   // struct Punto { ... } p1, p2;
    int vl = peek().line;
    std::string vn = take().text;
    return parseDeclRest(ty, false, vn, vl, global);
  }
  if (!expectOp(';')) return nullptr;
  return none;
}

bool Parser::parseTypedef() {
  int line = takeLine();
  int ty;
  if (isIdent("struct") && (isOp('{', 1) || (peek(1).type == TK_IDENT && isOp('{', 2)))) {
    uint8_t sty;
    if (!parseStructDef(true, true, sty)) return false;
    ty = sty;
  } else if (isIdent("enum")) {
    if (!parseEnum(true, true)) return false;
    ty = TY_I32;
  } else if (isIdent("union")) {
    failAt(line, "union no esta soportado");
    return false;
  } else {
    bool a, b, c;
    ty = parseType(a, b, false, c);
    if (failed) return false;
  }
  for (;;) {
    bool ptr = false;
    while (isOp('*')) { skip(); ptr = true; }
    std::string nm;
    if (!expectIdent(nm)) return false;
    typeAliases[nm] = ptr ? (uint8_t)TY_ANY : (uint8_t)ty;
    if (!acceptOp(',')) break;
  }
  return expectOp(';');
}

Node* Parser::parseInitList() {
  int line = peek().line;
  expectOp('{');
  Node* list = mk(N_INITLIST, line);
  if (!list) return nullptr;
  Node** tail = &list->a;
  int count = 0;
  while (!failed && !isOp('}')) {
    Node* e;
    if (isOp('.') && peek(1).type == TK_IDENT) {   // .campo = valor
      int dl = peek().line;
      skip();
      e = mk(N_DESIG, dl);
      if (!e) return nullptr;
      e->idx = P.intern(take().text);
      Node* cur = e;
      while (isOp('.') && peek(1).type == TK_IDENT) {   // .flags.output_invert = 1
        skip();
        Node* lst = mk(N_INITLIST, dl);
        Node* d2 = mk(N_DESIG, dl);
        if (!lst || !d2) return nullptr;
        d2->idx = P.intern(take().text);
        lst->a = d2;
        lst->ival = 1;
        cur->a = lst;
        cur = d2;
      }
      if (!expectOp('=')) return nullptr;
      cur->a = isOp('{') ? parseInitList() : parseAssign();
    } else if (isOp('[')) {
      failAt(peek().line, "los inicializadores [n] = valor no estan soportados");
      return nullptr;
    } else {
      e = isOp('{') ? parseInitList() : parseAssign();
    }
    if (failed) return nullptr;
    *tail = e;
    tail = &e->next;
    count++;
    if (!acceptOp(',')) break;
  }
  expectOp('}');
  list->ival = count;
  return failed ? nullptr : list;
}

Node* Parser::parseDeclarator(uint8_t ty, bool isStatic, const std::string& name, int line, bool global) {
  if (ty == TY_VOID) { failAt(line, "una variable no puede ser de tipo void"); return nullptr; }
  Node* dims = nullptr;
  Node** dtail = &dims;
  int ndims = 0;
  while (isOp('[')) {
    skip();
    Node* dn = mk(N_DIM, line);
    if (!dn) return nullptr;
    if (!isOp(']')) { dn->a = parseExpr(); if (failed) return nullptr; }
    if (!expectOp(']')) return nullptr;
    *dtail = dn;
    dtail = &dn->next;
    ndims++;
  }
  Node* init = nullptr;
  if (acceptOp('=')) {
    init = isOp('{') ? parseInitList() : parseAssign();
  } else if (isOp('{')) {
    init = parseInitList();
  } else if (isOp('(') && !ndims && ty == TY_SPISET) {  // SPISettings s(1000000, MSBFIRST, SPI_MODE0);
    skip();
    int argc;
    Node* args = parseArgs(argc);
    if (failed) return nullptr;
    init = mk(N_PCALL, line);
    if (!init) return nullptr;
    init->b = args;
    init->ival = argc;
    PendingCall pc;
    pc.n = init;
    pc.name = "SPISettings";
    pendingCalls.push_back(pc);
  } else if (isOp('(') && !ndims) {  // String s("hola");  int x(5);
    skip();
    if (!isOp(')')) init = parseAssign();
    expectOp(')');
  }
  if (failed) return nullptr;
  // char texto[] = "hola"  -> String
  if (ndims == 1 && ty == TY_CHAR && init && init->kind == N_STR) { dims = nullptr; ndims = 0; ty = TY_STRING; }
  if (ndims && init && init->kind != N_INITLIST && init->kind != N_STR) {
    failAt(line, "un array se inicializa con { ... }");
    return nullptr;
  }
  if (ndims && dims && !dims->a && !init) {
    failAt(line, "falta el tamano del array '%s'", name.c_str());
    return nullptr;
  }
  Node* d = mk(N_DECL, line);
  if (!d) return nullptr;
  d->op = ty;
  d->a = init;
  d->b = dims;
  if (global || (isStatic && inFunc)) {
    int slot = declareGlobal(name, ty, line, inFunc);
    d->idx = slot;
    d->ival = DF_GLOBAL;
    if (inFunc) {
      // variable static local: es global pero visible solo en este ambito
      LocalVar lv;
      lv.name = name; lv.slot = slot; lv.ty = ty; lv.depth = scopeDepth; lv.isGlobal = true;
      locals.push_back(lv);
    }
    P.globalInits.push_back(d);
    return d;
  }
  d->idx = declareLocal(name, ty, line);
  return d;
}

Node* Parser::parseDeclRest(uint8_t ty, bool isStatic, const std::string& firstName, int firstLine, bool global) {
  Node* blk = mk(N_BLOCK, firstLine);
  if (!blk) return nullptr;
  Node** tail = &blk->a;
  std::string name = firstName;
  int line = firstLine;
  for (;;) {
    Node* d = parseDeclarator(ty, isStatic, name, line, global);
    if (!d || failed) return nullptr;
    if (!(d->ival & DF_GLOBAL)) { *tail = d; tail = &d->next; }
    if (!acceptOp(',')) break;
    while (isOp('*')) skip();
    line = peek().line;
    if (!expectIdent(name)) return nullptr;
  }
  if (!expectOp(';')) return nullptr;
  return blk;
}

Node* Parser::parseBlock() {
  int line = peek().line;
  if (!expectOp('{')) return nullptr;
  if (++nest > MAX_PARSE_NEST) { failAt(line, "demasiados bloques anidados"); return nullptr; }
  enterScope();
  Node* blk = mk(N_BLOCK, line);
  if (!blk) return nullptr;
  Node** tail = &blk->a;
  while (!failed && !isOp('}')) {
    if (peek().type == TK_EOF) { failAt(line, "falta '}' para cerrar el bloque abierto aqui"); return nullptr; }
    Node* s = parseStatement();
    if (failed) return nullptr;
    if (s && s->kind != N_EMPTY) { *tail = s; tail = &s->next; }
  }
  expectOp('}');
  exitScope();
  nest--;
  return failed ? nullptr : blk;
}

Node* Parser::parseFor(int line) {
  if (!expectOp('(')) return nullptr;
  enterScope();
  Node* init = nullptr;
  if (isDeclStart()) {
    bool isStatic, isConst, isRef;
    int ty = parseType(isStatic, isConst, true, isRef);
    if (failed) return nullptr;
    int nline = peek().line;
    std::string name;
    if (!expectIdent(name)) return nullptr;
    if (acceptOp(':')) {  // for (int x : array)
      Node* arr = parseExpr();
      if (failed || !expectOp(')')) return nullptr;
      Node* fe = mk(N_FOREACH, line);
      if (!fe) return nullptr;
      fe->idx = declareLocal(name, (uint8_t)ty, nline);
      fe->op = (uint8_t)ty;
      fe->ival = isRef ? DF_REF : 0;
      fe->a = arr;
      loopDepth++;
      fe->b = parseStatement();
      loopDepth--;
      exitScope();
      return failed ? nullptr : fe;
    }
    if (isRef) { failAt(nline, "las referencias '&' solo se admiten en parametros"); return nullptr; }
    init = parseDeclRest((uint8_t)ty, false, name, nline, false);
    if (failed) return nullptr;
  } else if (!acceptOp(';')) {
    Node* e = parseExpr();
    if (failed) return nullptr;
    init = mk(N_EXPR, line);
    if (!init) return nullptr;
    init->a = e;
    if (!expectOp(';')) return nullptr;
  }
  Node* cond = nullptr;
  if (!isOp(';')) { cond = parseExpr(); if (failed) return nullptr; }
  if (!expectOp(';')) return nullptr;
  Node* step = nullptr;
  if (!isOp(')')) { step = parseExpr(); if (failed) return nullptr; }
  if (!expectOp(')')) return nullptr;
  loopDepth++;
  Node* body = parseStatement();
  loopDepth--;
  exitScope();
  if (failed) return nullptr;
  Node* f = mk(N_FOR, line);
  if (!f) return nullptr;
  f->a = init; f->b = cond; f->c = step; f->d = body;
  return f;
}

Node* Parser::parseStatement() {
  const Token& t = peek();
  int line = t.line;
  if (t.type == TK_OP) {
    if (t.op == '{') return parseBlock();
    if (t.op == ';') { skip(); return mk(N_EMPTY, line); }
  }
  if (t.type == TK_IDENT) {
    const std::string& s = t.text;
    if (s == "if") {
      skip();
      if (!expectOp('(')) return nullptr;
      Node* c = parseExpr();
      if (failed || !expectOp(')')) return nullptr;
      Node* n = mk(N_IF, line);
      if (!n) return nullptr;
      n->a = c;
      n->b = parseStatement();
      if (failed) return nullptr;
      if (isIdent("else")) { skip(); n->c = parseStatement(); }
      return failed ? nullptr : n;
    }
    if (s == "while") {
      skip();
      if (!expectOp('(')) return nullptr;
      Node* c = parseExpr();
      if (failed || !expectOp(')')) return nullptr;
      Node* n = mk(N_WHILE, line);
      if (!n) return nullptr;
      n->a = c;
      loopDepth++;
      n->b = parseStatement();
      loopDepth--;
      return failed ? nullptr : n;
    }
    if (s == "do") {
      skip();
      Node* n = mk(N_DOWHILE, line);
      if (!n) return nullptr;
      loopDepth++;
      n->b = parseStatement();
      loopDepth--;
      if (failed) return nullptr;
      if (!isIdent("while")) { failAt(peek().line, "falta 'while' despues del bloque 'do'"); return nullptr; }
      skip();
      if (!expectOp('(')) return nullptr;
      n->a = parseExpr();
      if (failed || !expectOp(')') || !expectOp(';')) return nullptr;
      return n;
    }
    if (s == "for") { skip(); return parseFor(line); }
    if (s == "switch") {
      skip();
      if (!expectOp('(')) return nullptr;
      Node* c = parseExpr();
      if (failed || !expectOp(')')) return nullptr;
      if (!isOp('{')) { failAt(peek().line, "se esperaba '{' despues de switch(...)"); return nullptr; }
      Node* n = mk(N_SWITCH, line);
      if (!n) return nullptr;
      n->a = c;
      switchDepth++;
      n->b = parseBlock();
      switchDepth--;
      return failed ? nullptr : n;
    }
    if (s == "case") {
      skip();
      if (!switchDepth) { failAt(line, "'case' fuera de un switch"); return nullptr; }
      Node* n = mk(N_CASE, line);
      if (!n) return nullptr;
      n->a = parseTernary();
      if (failed || !expectOp(':')) return nullptr;
      return n;
    }
    if (s == "default") {
      skip();
      if (!switchDepth) { failAt(line, "'default' fuera de un switch"); return nullptr; }
      if (!expectOp(':')) return nullptr;
      return mk(N_DEFAULT, line);
    }
    if (s == "break") {
      skip();
      if (!loopDepth && !switchDepth) { failAt(line, "'break' fuera de un bucle o switch"); return nullptr; }
      if (!expectOp(';')) return nullptr;
      return mk(N_BREAK, line);
    }
    if (s == "continue") {
      skip();
      if (!loopDepth) { failAt(line, "'continue' fuera de un bucle"); return nullptr; }
      if (!expectOp(';')) return nullptr;
      return mk(N_CONTINUE, line);
    }
    if (s == "return") {
      skip();
      Node* n = mk(N_RETURN, line);
      if (!n) return nullptr;
      if (!isOp(';')) { n->a = parseExpr(); if (failed) return nullptr; }
      if (!expectOp(';')) return nullptr;
      return n;
    }
    if (s == "enum") { parseEnum(false); return failed ? nullptr : mk(N_EMPTY, line); }
    if (s == "typedef") { parseTypedef(); return failed ? nullptr : mk(N_EMPTY, line); }
    if (s == "goto") { failAt(line, "'goto' no esta soportado"); return nullptr; }
    if (s == "struct" && (isOp('{', 1) || (peek(1).type == TK_IDENT && isOp('{', 2)))) {
      uint8_t sty;
      return parseStructDef(false, false, sty);
    }
    if (s == "class") { failAt(line, "'class' no esta soportado por el interprete (usa struct)"); return nullptr; }
    if (isDeclStart()) {
      bool isStatic, isConst, isRef;
      int ty = parseType(isStatic, isConst, false, isRef);
      if (failed) return nullptr;
      int nline = peek().line;
      std::string name;
      if (!expectIdent(name)) return nullptr;
      if (isOp('(')) {
        const Token& n1 = peek(1);
        if ((n1.type == TK_OP && n1.op == ')') || (n1.type == TK_IDENT && isTypeWord(n1.text))) {
          failAt(nline, "la funcion '%s' esta dentro de otra funcion: seguramente falta una '}' antes", name.c_str());
          return nullptr;
        }
      }
      Node* blk = parseDeclRest((uint8_t)ty, isStatic, name, nline, false);
      if (failed) return nullptr;
      if (!blk->a) return mk(N_EMPTY, line);
      return blk;
    }
    if (t.type == TK_IDENT && peek(1).type == TK_IDENT && !isTypeWord(s)) {
      failAt(line, "tipo desconocido '%s' (las librerias y clases externas no estan disponibles)", s.c_str());
      return nullptr;
    }
  }
  Node* e = parseExpr();
  if (failed) return nullptr;
  if (!expectOp(';')) return nullptr;
  Node* n = mk(N_EXPR, line);
  if (!n) return nullptr;
  n->a = e;
  return n;
}

// ---- expresiones ----
Node* Parser::parseExpr() {
  Node* e = parseAssign();
  while (!failed && isOp(',')) {
    int line = takeLine();
    Node* r = parseAssign();
    if (failed) return nullptr;
    Node* c = mk(N_COMMA, line);
    if (!c) return nullptr;
    c->a = e; c->b = r;
    e = c;
  }
  return failed ? nullptr : e;
}

static int assignOpOf(const Token& t) {
  if (t.type != TK_OP) return 0;
  switch (t.op) {
    case '=': return '=';
    case OP_ADDEQ: return '+';
    case OP_SUBEQ: return '-';
    case OP_MULEQ: return '*';
    case OP_DIVEQ: return '/';
    case OP_MODEQ: return '%';
    case OP_ANDEQ: return '&';
    case OP_OREQ: return '|';
    case OP_XOREQ: return '^';
    case OP_SHLEQ: return 'L';
    case OP_SHREQ: return 'R';
    default: return 0;
  }
}

Node* Parser::parseAssign() {
  if (++nest > MAX_PARSE_NEST) { failAt(peek().line, "expresion demasiado anidada"); return nullptr; }
  Node* lhs = parseTernary();
  if (failed) return nullptr;
  int aop = assignOpOf(peek());
  if (aop) {
    int line = takeLine();
    if (undeclared(lhs, line)) return nullptr;
    if (!isLvalue(lhs)) { failAt(line, "solo se puede asignar a una variable o a un elemento de array"); return nullptr; }
    Node* rhs = parseAssign();
    if (failed) return nullptr;
    Node* n = mk(N_ASSIGN, line);
    if (!n) return nullptr;
    n->op = (uint8_t)aop;
    n->a = lhs; n->b = rhs;
    lhs = n;
  }
  nest--;
  return lhs;
}

Node* Parser::parseTernary() {
  Node* c = parseBinary(4);
  if (failed) return nullptr;
  if (isOp('?')) {
    int line = takeLine();
    Node* a = parseAssign();
    if (failed || !expectOp(':')) return nullptr;
    Node* b = parseAssign();
    if (failed) return nullptr;
    Node* n = mk(N_TERN, line);
    if (!n) return nullptr;
    n->a = c; n->b = a; n->c = b;
    return n;
  }
  return c;
}

int Parser::binPrec(const Token& t) {
  if (t.type != TK_OP) return -1;
  switch (t.op) {
    case '*': case '/': case '%': return 13;
    case '+': case '-': return 12;
    case OP_SHL: case OP_SHR: return 11;
    case '<': case '>': case OP_LE: case OP_GE: return 10;
    case OP_EQ: case OP_NE: return 9;
    case '&': return 8;
    case '^': return 7;
    case '|': return 6;
    case OP_ANDAND: return 5;
    case OP_OROR: return 4;
    default: return -1;
  }
}

Node* Parser::parseBinary(int minPrec) {
  Node* lhs = parseUnary();
  if (failed) return nullptr;
  for (;;) {
    const Token& t = peek();
    int p = binPrec(t);
    if (p < minPrec) break;
    int op = t.op;
    int line = takeLine();
    Node* rhs = parseBinary(p + 1);
    if (failed) return nullptr;
    Node* n;
    if (op == OP_ANDAND) n = mk(N_AND, line);
    else if (op == OP_OROR) n = mk(N_OR, line);
    else n = mk(N_BIN, line);
    if (!n) return nullptr;
    n->idx = op;
    n->op = (uint8_t)(op < 256 ? op : 0);
    n->a = lhs; n->b = rhs;
    lhs = n;
  }
  return lhs;
}

Node* Parser::parseUnary() {
  const Token& t = peek();
  int line = t.line;
  if (t.type == TK_OP) {
    int kind = -1;
    switch (t.op) {
      case '-': kind = N_NEG; break;
      case '+': kind = N_POS; break;
      case '!': kind = N_NOT; break;
      case '~': kind = N_BITNOT; break;
      case OP_INC: kind = N_PREINC; break;
      case OP_DEC: kind = N_PREDEC; break;
      case '*': failAt(line, "los punteros (*p) no estan soportados; usa arrays"); return nullptr;
      case '&': {
        skip();
        Node* a = parseUnary();
        if (failed) return nullptr;
        if (a->kind == N_FUNCREF) return a;   // &miFuncion (interrupciones)
        Node* n = mk(N_ADDR, line);
        if (n) n->a = a;
        return n;
      }
      default: break;
    }
    if (kind >= 0) {
      skip();
      if (++nest > MAX_PARSE_NEST) { failAt(line, "expresion demasiado anidada"); return nullptr; }
      Node* a = parseUnary();
      nest--;
      if (failed) return nullptr;
      if ((kind == N_PREINC || kind == N_PREDEC) && undeclared(a, line)) return nullptr;
      if ((kind == N_PREINC || kind == N_PREDEC) && !isLvalue(a)) {
        failAt(line, "++ y -- solo funcionan con variables");
        return nullptr;
      }
      // constantes negativas directas
      if (kind == N_NEG && a->kind == N_NUM) { a->ival = -a->ival; a->op = 0; return a; }
      if (kind == N_NEG && a->kind == N_FLT) { a->fval = -a->fval; return a; }
      Node* n = mk((uint8_t)kind, line);
      if (!n) return nullptr;
      n->a = a;
      return n;
    }
  }
  Node* p = parsePrimary();
  if (failed) return nullptr;
  return parsePostfix(p);
}

Node* Parser::parseArgs(int& argc) {
  // '(' ya consumido
  argc = 0;
  Node* first = nullptr;
  Node** tail = &first;
  while (!failed && !isOp(')')) {
    Node* a = parseAssign();
    if (failed) return nullptr;
    *tail = a;
    tail = &a->next;
    argc++;
    if (!acceptOp(',')) break;
  }
  expectOp(')');
  if (argc > MAX_ARGS) failAt(prevLine, "demasiados argumentos (max %d)", MAX_ARGS);
  return first;
}

Node* Parser::parsePostfix(Node* e) {
  for (;;) {
    if (failed) return nullptr;
    const Token& t = peek();
    if (t.type != TK_OP) return e;
    int line = t.line;
    if (t.op == '[') {
      skip();
      Node* idx = parseExpr();
      if (failed || !expectOp(']')) return nullptr;
      Node* n = mk(N_INDEX, line);
      if (!n) return nullptr;
      n->a = e; n->b = idx;
      e = n;
      continue;
    }
    if (t.op == OP_INC || t.op == OP_DEC) {
      if (undeclared(e, line)) return nullptr;
      if (!isLvalue(e)) { failAt(line, "++ y -- solo funcionan con variables"); return nullptr; }
      Node* n = mk(t.op == OP_INC ? N_POSTINC : N_POSTDEC, line);
      skip();
      if (!n) return nullptr;
      n->a = e;
      e = n;
      continue;
    }
    if (t.op == '.' || t.op == OP_ARROW) {
      skip();
      std::string mname;
      if (!expectIdent(mname)) return nullptr;
      if (undeclared(e, line)) return nullptr;
      if (!isOp('(')) {   // campo de una estructura: x.campo
        Node* n = mk(N_FIELD, line);
        if (!n) return nullptr;
        n->a = e;
        n->idx = P.intern(mname);
        e = n;
        continue;
      }
      if (staticType(e) == TY_SERVO) {
        std::string full = "Servo." + mname;
        if (findBuiltin(full) < 0) { failAt(line, "Servo no tiene el metodo '.%s()'", mname.c_str()); return nullptr; }
        if (!isLvalue(e)) { failAt(line, "los metodos de Servo se usan sobre una variable Servo"); return nullptr; }
        if (!expectOp('(')) return nullptr;
        int argc;
        Node* args = parseArgs(argc);
        if (failed) return nullptr;
        Node* n = mk(N_PCALL, line);
        if (!n) return nullptr;
        e->next = args;
        n->b = e;
        n->ival = argc + 1;
        PendingCall pc;
        pc.n = n;
        pc.name = full;
        pendingCalls.push_back(pc);
        e = n;
        continue;
      }
      int mid = findMethod(mname);
      if (mid < 0) { failAt(line, "el metodo '.%s()' no esta soportado", mname.c_str()); return nullptr; }
      if (!expectOp('(')) return nullptr;
      int argc;
      Node* args = parseArgs(argc);
      if (failed) return nullptr;
      if (mid >= M_CONCAT && !isLvalue(e)) {
        failAt(line, ".%s() modifica el String: usalo sobre una variable", mname.c_str());
        return nullptr;
      }
      Node* n = mk(N_METHOD, line);
      if (!n) return nullptr;
      n->a = e; n->b = args; n->idx = mid; n->ival = argc;
      e = n;
      continue;
    }
    if (t.op == '(') {
      failAt(line, "no se puede llamar a esta expresion como funcion");
      return nullptr;
    }
    return e;
  }
}

// Constantes de ESP-IDF iguales en todos los chips (las que cambian las da el firmware)
static bool numSuffix(const std::string& s, const char* pre, const char* post, int lo, int hi, int64_t& v) {
  size_t lp = strlen(pre), lq = strlen(post);
  if (s.size() <= lp + lq || s.compare(0, lp, pre) != 0 || s.compare(s.size() - lq, lq, post) != 0) return false;
  int x = 0;
  for (size_t k = lp; k < s.size() - lq; k++) {
    if (!isdigit((unsigned char)s[k]) || x > 1000) return false;
    x = x * 10 + (s[k] - '0');
  }
  if (x < lo || x > hi) return false;
  v = x;
  return true;
}

static bool idfConstant(const std::string& s, int64_t& v) {
  struct C { const char* n; int64_t v; };
  static const C tbl[] = {
    {"LEDC_INTR_DISABLE", 0}, {"LEDC_INTR_FADE_END", 1}, {"LEDC_FADE_NO_WAIT", 0}, {"LEDC_FADE_WAIT_DONE", 1},
    {"LEDC_DUTY_DIR_DECREASE", 0}, {"LEDC_DUTY_DIR_INCREASE", 1}, {"LEDC_SLEEP_MODE_NO_ALIVE_NO_PD", 0},
    {"LEDC_SLEEP_MODE_NO_ALIVE_ALLOW_PD", 1}, {"LEDC_SLEEP_MODE_KEEP_ALIVE", 2},
    {"ESP_OK", 0}, {"ESP_FAIL", -1}, {"ESP_ERR_NO_MEM", 0x101}, {"ESP_ERR_INVALID_ARG", 0x102},
    {"ESP_ERR_INVALID_STATE", 0x103}, {"ESP_ERR_INVALID_SIZE", 0x104}, {"ESP_ERR_NOT_FOUND", 0x105},
    {"ESP_ERR_NOT_SUPPORTED", 0x106}, {"ESP_ERR_TIMEOUT", 0x107},
    {"GPIO_NUM_NC", -1}, {"GPIO_MODE_DISABLE", 0}, {"GPIO_MODE_INPUT", 1}, {"GPIO_MODE_OUTPUT", 2},
    {"GPIO_MODE_OUTPUT_OD", 6}, {"GPIO_MODE_INPUT_OUTPUT_OD", 7}, {"GPIO_MODE_INPUT_OUTPUT", 3},
    {"GPIO_PULLUP_DISABLE", 0}, {"GPIO_PULLUP_ENABLE", 1}, {"GPIO_PULLDOWN_DISABLE", 0}, {"GPIO_PULLDOWN_ENABLE", 1},
    {"GPIO_INTR_DISABLE", 0}, {"GPIO_INTR_POSEDGE", 1}, {"GPIO_INTR_NEGEDGE", 2}, {"GPIO_INTR_ANYEDGE", 3},
    {"GPIO_INTR_LOW_LEVEL", 4}, {"GPIO_INTR_HIGH_LEVEL", 5}, {"GPIO_PULLUP_ONLY", 0}, {"GPIO_PULLDOWN_ONLY", 1},
    {"GPIO_PULLUP_PULLDOWN", 2}, {"GPIO_FLOATING", 3},
    {"portTICK_PERIOD_MS", 1}, {"portTICK_RATE_MS", 1}, {"configTICK_RATE_HZ", 1000}, {"pdTRUE", 1},
    {"pdFALSE", 0}, {"pdPASS", 1}, {"pdFAIL", 0}, {"portMAX_DELAY", 4294967295LL},
    {"ESP_INTR_FLAG_LEVEL1", 2}, {"ESP_INTR_FLAG_LEVEL2", 4}, {"ESP_INTR_FLAG_LEVEL3", 8},
    {"ESP_INTR_FLAG_SHARED", 256}, {"ESP_INTR_FLAG_IRAM", 1024}, {nullptr, 0}};
  for (int k = 0; tbl[k].n; k++)
    if (s == tbl[k].n) { v = tbl[k].v; return true; }
  return numSuffix(s, "LEDC_CHANNEL_", "", 0, 7, v) || numSuffix(s, "LEDC_TIMER_", "_BIT", 1, 20, v) ||
         numSuffix(s, "LEDC_TIMER_", "", 0, 3, v) || numSuffix(s, "GPIO_NUM_", "", 0, 56, v);
}

static const char* espErrName(int64_t c) {
  switch (c) {
    case 0: return "ESP_OK";
    case -1: return "ESP_FAIL";
    case 0x101: return "ESP_ERR_NO_MEM";
    case 0x102: return "ESP_ERR_INVALID_ARG (algun valor no es valido)";
    case 0x103: return "ESP_ERR_INVALID_STATE";
    case 0x104: return "ESP_ERR_INVALID_SIZE";
    case 0x105: return "ESP_ERR_NOT_FOUND";
    case 0x106: return "ESP_ERR_NOT_SUPPORTED";
    case 0x107: return "ESP_ERR_TIMEOUT";
    default: return "ERROR";
  }
}

Node* Parser::parseIdentifier(const Token& t) {
  const std::string& s = t.text;
  int line = t.line;
  // variables locales (de dentro hacia fuera)
  for (size_t k = locals.size(); k-- > 0;) {
    if (locals[k].name == s) {
      Node* n = mk(locals[k].isGlobal ? N_GVAR : N_LVAR, line);
      if (!n) return nullptr;
      n->idx = locals[k].slot;
      n->op = locals[k].ty;
      return n;
    }
  }
  for (size_t k = gvars.size(); k-- > 0;) {
    if (gvars[k].name == s) {
      Node* n = mk(N_GVAR, line);
      if (!n) return nullptr;
      n->idx = gvars[k].slot;
      n->op = gvars[k].ty;
      return n;
    }
  }
  std::map<std::string, int64_t>::iterator ec = enumConsts.find(s);
  if (ec != enumConsts.end()) {
    Node* n = mk(N_NUM, line);
    if (n) n->ival = ec->second;
    return n;
  }
  struct KConst { const char* n; int64_t v; };
  static const KConst ki[] = {
    {"HIGH", 1}, {"LOW", 0}, {"INPUT", MC_INPUT}, {"OUTPUT", MC_OUTPUT}, {"INPUT_PULLUP", MC_INPUT_PULLUP},
    {"INPUT_PULLDOWN", MC_INPUT_PULLDOWN}, {"OUTPUT_OPEN_DRAIN", MC_OUTPUT_OPEN_DRAIN}, {"true", 1},
    {"false", 0}, {"DEC", 10}, {"HEX", 16}, {"OCT", 8}, {"BIN", 2}, {"LSBFIRST", 0}, {"MSBFIRST", 1},
    {"NULL", 0}, {"nullptr", 0}, {"RISING", 1}, {"FALLING", 2}, {"CHANGE", 3}, {"ONLOW", 4}, {"ONHIGH", 5},
    {"ONLOW_WE", 0x0C}, {"ONHIGH_WE", 0x0D}, {"SPI_MODE0", 0}, {"SPI_MODE1", 1}, {"SPI_MODE2", 2},
    {"SPI_MODE3", 3}, {"SPI_MSBFIRST", 1}, {"SPI_LSBFIRST", 0}, {"ADC_0db", 0}, {"ADC_2_5db", 1},
    {"ADC_6db", 2}, {"ADC_11db", 3}, {"ADC_ATTEN_DB_0", 0}, {"ADC_ATTEN_DB_2_5", 1}, {"ADC_ATTEN_DB_6", 2},
    {"ADC_ATTEN_DB_11", 3}, {"ADC_ATTEN_DB_12", 3}, {"portMUX_INITIALIZER_UNLOCKED", 0}, {"NOTE_C", 0},
    {"NOTE_Cs", 1}, {"NOTE_D", 2}, {"NOTE_Eb", 3}, {"NOTE_E", 4}, {"NOTE_F", 5}, {"NOTE_Fs", 6}, {"NOTE_G", 7},
    {"NOTE_Gs", 8}, {"NOTE_A", 9}, {"NOTE_Bb", 10}, {"NOTE_B", 11}, {"RGB_BRIGHTNESS", 64},
    {"MIN_PULSE_WIDTH", 544}, {"MAX_PULSE_WIDTH", 2400}, {"INT_MAX", 2147483647LL},
    {"INT_MIN", -2147483647LL - 1}, {"UINT32_MAX", 4294967295LL}, {"LONG_MAX", 2147483647LL}, {nullptr, 0}};
  for (int k = 0; ki[k].n; k++) {
    if (s == ki[k].n) {
      Node* n = mk(N_NUM, line);
      if (n) n->ival = ki[k].v;
      return n;
    }
  }
  if (s == "LED_BUILTIN" || s == "RGB_BUILTIN") {
    Node* n = mk(N_NUM, line);
    if (n) n->ival = P.hal.ledBuiltin();
    return n;
  }
  struct FConst { const char* n; double v; };
  static const FConst kf[] = {
    {"PI", 3.14159265358979323846}, {"HALF_PI", 1.57079632679489661923}, {"TWO_PI", 6.28318530717958647693},
    {"DEG_TO_RAD", 0.017453292519943295769}, {"RAD_TO_DEG", 57.295779513082320876}, {"EULER", 2.718281828459045235},
    {"M_PI", 3.14159265358979323846}, {nullptr, 0}};
  for (int k = 0; kf[k].n; k++) {
    if (s == kf[k].n) {
      Node* n = mk(N_FLT, line);
      if (n) n->fval = kf[k].v;
      return n;
    }
  }
  {
    int64_t cv;
    if (idfConstant(s, cv)) {
      Node* n = mk(N_NUM, line);
      if (n) n->ival = cv;
      return n;
    }
  }
  {
    int pc = P.hal.pinConstant(s);  // SDA, SCL, MOSI, DAC1, LEDC_LOW_SPEED_MODE...
    if (pc >= 0) {
      Node* n = mk(N_NUM, line);
      if (n) n->ival = pc;
      return n;
    }
  }
  // puede ser el nombre de una funcion (attachInterrupt(pin, miFuncion, RISING))
  Node* fr = mk(N_FUNCREF, line);
  if (!fr) return nullptr;
  PendingCall pc;
  pc.n = fr;
  pc.name = s;
  pendingCalls.push_back(pc);
  return fr;
}

Node* Parser::parseStringLit(int line, const std::string& first) {
  std::string s = first;
  while (peek().type == TK_STRING) s += take().text;
  Node* n = mk(N_STR, line);
  if (!n) return nullptr;
  n->idx = (int32_t)P.strConsts.size();
  P.strConsts.push_back(Value::S(s));
  return n;
}

Node* Parser::parseCCast(int line) {
  bool st, cn, rf;
  int ty = parseType(st, cn, false, rf);
  if (failed || !expectOp(')')) return nullptr;
  if (++nest > MAX_PARSE_NEST) { failAt(line, "expresion demasiado anidada"); return nullptr; }
  Node* a = parseUnary();
  nest--;
  if (failed) return nullptr;
  Node* n = mk(N_CAST, line);
  if (!n) return nullptr;
  n->op = (uint8_t)ty; n->a = a;
  return n;
}

Node* Parser::parseParen(int line) {
  // '(' ya consumido
  if (peek().type == TK_IDENT && isTypeWord(peek().text) && !isOp(OP_SCOPE, 1)) return parseCCast(line);
  Node* e = parseExpr();
  if (failed || !expectOp(')')) return nullptr;
  return e;
}

Node* Parser::parsePrimary() {
  const Token& t = peek();
  int line = t.line;
  switch (t.type) {
    case TK_INT: { int64_t v = t.ival; skip(); Node* n = mk(N_NUM, line); if (n) n->ival = v; return n; }
    case TK_CHAR: { int64_t v = t.ival; skip(); Node* n = mk(N_NUM, line); if (n) { n->ival = v; n->op = 1; } return n; }
    case TK_FLOAT: { double v = t.fval; skip(); Node* n = mk(N_FLT, line); if (n) n->fval = v; return n; }
    case TK_STRING: { Token st = take(); return parseStringLit(line, st.text); }
    case TK_EOF:
      failAt(line, "el programa termina de forma inesperada (falta algo)");
      return nullptr;
    case TK_OP:
      if (t.op == '(') { skip(); return parseParen(line); }
      {
        std::string d = desc(t);
        failAt(line, "expresion no valida: no se esperaba %s", d.c_str());
      }
      return nullptr;
    case TK_IDENT: {
      Token id = take();
      return parseIdentExpr(id);
    }
    default:
      failAt(line, "expresion no valida");
      return nullptr;
  }
}

Node* Parser::parseIdentExpr(const Token& t) {
  int line = t.line;
  const std::string& s = t.text;
  if (s == "sizeof") {
    if (!expectOp('(')) return nullptr;
    Node* n = mk(N_SIZEOF, line);
    if (!n) return nullptr;
    if (peek().type == TK_IDENT && isTypeWord(peek().text)) {
      bool st, cn, rf;
      n->op = (uint8_t)parseType(st, cn, false, rf);
      n->a = nullptr;
    } else {
      n->a = parseExpr();
    }
    if (failed || !expectOp(')')) return nullptr;
    return n;
  }
  if (s == "SPISettings" && isOp('(')) {
    skip();
    int argc;
    Node* args = parseArgs(argc);
    if (failed) return nullptr;
    Node* n = mk(N_PCALL, line);
    if (!n) return nullptr;
    n->b = args; n->ival = argc;
    PendingCall pc; pc.n = n; pc.name = s;
    pendingCalls.push_back(pc);
    return n;
  }
  // conversion estilo funcion: int(x), String(x), String(x, HEX)
  if (isTypeWord(s) && isOp('(')) {
    uint8_t ty;
    if (s == "String") ty = TY_STRING;
    else if (s == "int" || s == "long" || s == "short") ty = TY_I32;
    else if (s == "unsigned") ty = TY_U32;
    else if (s == "float") ty = TY_FLOAT;
    else if (s == "double") ty = TY_DOUBLE;
    else if (s == "char") ty = TY_CHAR;
    else if (s == "byte" || s == "uint8_t") ty = TY_U8;
    else if (s == "bool" || s == "boolean") ty = TY_BOOL;
    else if (s == "word" || s == "uint16_t") ty = TY_U16;
    else if (s == "int16_t") ty = TY_I16;
    else if (s == "int8_t") ty = TY_I8;
    else if (s == "uint32_t" || s == "size_t") ty = TY_U32;
    else if (s == "int32_t") ty = TY_I32;
    else if (s == "int64_t") ty = TY_I64;
    else if (s == "uint64_t") ty = TY_U64;
    else if (typeAliases.count(s)) ty = typeAliases[s];
    else { failAt(line, "conversion '%s(...)' no valida", s.c_str()); return nullptr; }
    skip();  // (
    int argc;
    Node* args = parseArgs(argc);
    if (failed) return nullptr;
    if (argc == 2 && ty == TY_STRING) {
      Node* n = mk(N_BUILTIN, line);
      if (!n) return nullptr;
      n->idx = B_STRING2; n->b = args; n->ival = 2;
      return n;
    }
    if (argc > 1) { failAt(line, "%s(...) solo admite un valor", s.c_str()); return nullptr; }
    Node* n = mk(N_CAST, line);
    if (!n) return nullptr;
    n->op = ty;
    if (argc == 0) {
      Node* z;
      if (ty == TY_STRING) {
        z = mk(N_STR, line);
        if (!z) return nullptr;
        z->idx = (int32_t)P.strConsts.size();
        P.strConsts.push_back(Value::S(std::string()));
      } else {
        z = mk(N_NUM, line);
        if (!z) return nullptr;
      }
      n->a = z;
    } else {
      n->a = args;
    }
    return n;
  }
  // Objetos integrados: Serial.x(), Wire.x(), Wire1.x(), SPI.x(), ESP.x()
  {
    const char* ns = nullptr;
    int bus = 0;
    if (s == "Serial" || s == "Serial0" || s == "USBSerial") ns = "Serial";
    else if (s == "Wire") ns = "Wire";
    else if (s == "Wire1") { ns = "Wire"; bus = 1; }
    else if (s == "SPI") ns = "SPI";
    else if (s == "ESP") ns = "ESP";
    if (ns) {
      if (isOp('.')) {
        skip();
        std::string m;
        if (!expectIdent(m)) return nullptr;
        std::string full = std::string(ns) + "." + m;
        if (findBuiltin(full) < 0) { failAt(line, "%s.%s() no esta soportado", s.c_str(), m.c_str()); return nullptr; }
        if (!expectOp('(')) return nullptr;
        int argc;
        Node* args = parseArgs(argc);
        if (failed) return nullptr;
        Node* n = mk(N_PCALL, line);
        if (!n) return nullptr;
        n->b = args; n->ival = argc; n->op = (uint8_t)bus;
        PendingCall pc; pc.n = n; pc.name = full;
        pendingCalls.push_back(pc);
        return n;
      }
      if (ns[0] == 'S' && ns[1] == 'e') {  // while(!Serial)
        Node* n = mk(N_NUM, line);
        if (n) n->ival = 1;
        return n;
      }
      failAt(line, "'%s' se usa con un punto, por ejemplo %s.begin()", s.c_str(), s.c_str());
      return nullptr;
    }
  }
  // Nombre::VALOR (enum class)
  if (isOp(OP_SCOPE) && typeAliases.count(s)) {
    skip();
    Token v = take();
    if (v.type != TK_IDENT) { failAt(line, "se esperaba un nombre despues de '::'"); return nullptr; }
    return parseIdentifier(v);
  }
  // llamada a funcion
  if (isOp('(')) {
    skip();
    int argc;
    Node* args = parseArgs(argc);
    if (failed) return nullptr;
    Node* n = mk(N_PCALL, line);
    if (!n) return nullptr;
    n->b = args; n->ival = argc;
    PendingCall pc; pc.n = n; pc.name = s;
    pendingCalls.push_back(pc);
    return n;
  }
  return parseIdentifier(t);
}

bool Parser::resolveCalls() {
  for (size_t k = 0; k < pendingCalls.size(); k++) {
    Node* n = pendingCalls[k].n;
    const std::string& name = pendingCalls[k].name;
    int argc = (int)n->ival;
    int fi = -1;
    for (size_t q = 0; q < P.funcs.size(); q++)
      if (P.funcs[q].name == name) { fi = (int)q; break; }
    if (n->kind == N_FUNCREF) {
      if (fi < 0 || !P.funcs[fi].defined) { failAt(n->line, "'%s' no esta declarado", name.c_str()); return false; }
      n->idx = fi;
      continue;
    }
    if (fi >= 0) {
      Func& f = P.funcs[fi];
      if (!f.defined) { failAt(n->line, "la funcion '%s' esta declarada pero no tiene cuerpo", name.c_str()); return false; }
      int np = (int)f.params.size();
      int required = np;
      while (required > 0 && f.params[required - 1].def) required--;
      if (argc < required || argc > np) {
        failAt(n->line, "'%s' necesita %d argumento(s) y se le pasan %d", name.c_str(), np, argc);
        return false;
      }
      n->kind = N_CALL;
      n->idx = fi;
      continue;
    }
    int bi = findBuiltin(name);
    if (bi >= 0) {
      int lo, hi;
      const char* bname;
      builtinRange(bi, lo, hi, bname);
      int shown = argc;
      if (bi >= B__COUNT && (kNatives[bi - B__COUNT].flags & XF_SELF)) { lo--; hi--; shown--; }
      if (shown < lo || shown > hi) {
        if (lo == hi) failAt(n->line, "%s() necesita %d argumento(s) y se le pasan %d", name.c_str(), lo, shown);
        else failAt(n->line, "%s() necesita entre %d y %d argumentos y se le pasan %d", name.c_str(), lo, hi, shown);
        return false;
      }
      if ((bi == B_BITSET || bi == B_BITCLEAR || bi == B_BITWRITE) && !isLvalue(n->b)) {
        failAt(n->line, "%s() necesita una variable como primer argumento", name.c_str());
        return false;
      }
      n->kind = N_BUILTIN;
      n->idx = bi;
      continue;
    }
    if (name == "sprintf" || name == "snprintf" || name == "strcpy" || name == "strcat" || name == "malloc")
      failAt(n->line, "%s() no esta soportado; usa String", name.c_str());
    else
      failAt(n->line, "la funcion '%s' no existe", name.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------
//  Evaluacion
// ---------------------------------------------------------------------
Value* Impl::lref(Node* n, uint8_t& ty) {
  switch (n->kind) {
    case N_LVAR: {
      Value* v = &stack[fp + n->idx];
      if (v->t == V_REF) v = v->r;
      ty = n->op;
      return v;
    }
    case N_GVAR:
      ty = n->op;
      return &globals[n->idx];
    case N_INDEX: {
      Value base = eval(n->a);
      if (abort_) return nullptr;
      Value iv = eval(n->b);
      if (abort_) return nullptr;
      if (base.t != V_ARR) {
        if (base.t == V_STR) fail(n->line, "no se puede cambiar una letra de un String con [ ]; usa .setCharAt()");
        else fail(n->line, "se usa [ ] sobre algo que no es un array");
        return nullptr;
      }
      if (!iv.isNum()) { fail(n->line, "el indice del array debe ser un numero"); return nullptr; }
      int64_t i = iv.t == V_INT ? iv.i : f2i(iv.f);
      ArrObj* arr = base.a;
      if (i < 0 || i >= (int64_t)arr->v.size()) {
        fail(n->line, "indice %lld fuera del array (tamano %d)", (long long)i, (int)arr->v.size());
        return nullptr;
      }
      ty = arr->elemTy;
      lrefHold = std::move(base);
      return &arr->v[(size_t)i];
    }
    case N_FIELD: {
      Value base = eval(n->a);
      if (abort_) return nullptr;
      Value* p = fieldOf(base, n, ty);
      if (!p) return nullptr;
      lrefHold = std::move(base);
      return p;
    }
    default:
      fail(n->line, "no se puede asignar a esta expresion");
      return nullptr;
  }
}

Value Impl::binop(int op, const Value& a, const Value& b, int line) {
  if (a.t == V_STR || b.t == V_STR) {
    if (op == '+') return Value::S(toStr(a) + toStr(b));
    if (a.t == V_STR && b.t == V_STR) {
      int c = a.s->s.compare(b.s->s);
      switch (op) {
        case OP_EQ: return Value::I(c == 0);
        case OP_NE: return Value::I(c != 0);
        case '<': return Value::I(c < 0);
        case '>': return Value::I(c > 0);
        case OP_LE: return Value::I(c <= 0);
        case OP_GE: return Value::I(c >= 0);
        default: break;
      }
    }
    if ((op == OP_EQ || op == OP_NE) && (a.isNum() || b.isNum())) {
      const Value& sv = a.t == V_STR ? a : b;
      const Value& nv = a.t == V_STR ? b : a;
      bool eq = nv.t == V_INT && nv.isChar && sv.s->s.size() == 1 && sv.s->s[0] == (char)nv.i;
      return Value::I(op == OP_EQ ? eq : !eq);
    }
    fail(line, "operacion '%s' no valida con String", opText(op));
    return Value();
  }
  if (!a.isNum() || !b.isNum()) {
    fail(line, "operacion '%s' con %s", opText(op), kindName(a.isNum() ? b : a));
    return Value();
  }
  if (a.t == V_FLT || b.t == V_FLT) {
    double x = num(a), y = num(b);
    switch (op) {
      case '+': return Value::F(x + y);
      case '-': return Value::F(x - y);
      case '*': return Value::F(x * y);
      case '/': return Value::F(x / y);
      case '%': return Value::F(fmod(x, y));
      case '<': return Value::I(x < y);
      case '>': return Value::I(x > y);
      case OP_LE: return Value::I(x <= y);
      case OP_GE: return Value::I(x >= y);
      case OP_EQ: return Value::I(x == y);
      case OP_NE: return Value::I(x != y);
      default:
        fail(line, "el operador '%s' solo funciona con enteros, no con float", opText(op));
        return Value();
    }
  }
  int64_t x = a.i, y = b.i;
  switch (op) {
    case '+': return Value::I((int64_t)((uint64_t)x + (uint64_t)y));
    case '-': return Value::I((int64_t)((uint64_t)x - (uint64_t)y));
    case '*': return Value::I((int64_t)((uint64_t)x * (uint64_t)y));
    case '/':
      if (y == 0) { fail(line, "division entre cero"); return Value(); }
      if (y == -1) return Value::I((int64_t)(0 - (uint64_t)x));
      return Value::I(x / y);
    case '%':
      if (y == 0) { fail(line, "modulo (%%) entre cero"); return Value(); }
      if (y == -1) return Value::I(0);
      return Value::I(x % y);
    case '&': return Value::I(x & y);
    case '|': return Value::I(x | y);
    case '^': return Value::I(x ^ y);
    case OP_SHL: return Value::I((y < 0 || y > 63) ? 0 : (int64_t)((uint64_t)x << y));
    case OP_SHR: return Value::I((y < 0 || y > 63) ? (x < 0 ? -1 : 0) : (x >> y));
    case '<': return Value::I(x < y);
    case '>': return Value::I(x > y);
    case OP_LE: return Value::I(x <= y);
    case OP_GE: return Value::I(x >= y);
    case OP_EQ: return Value::I(x == y);
    case OP_NE: return Value::I(x != y);
    default:
      fail(line, "operador desconocido");
      return Value();
  }
}

Value Impl::assign(Node* n) {
  Value rhs = eval(n->b);
  if (abort_) return Value();
  uint8_t ty;
  Value* dst = lref(n->a, ty);
  if (!dst) return Value();
  if (dst->t == V_ARR && !(dst->a->sid >= 0 && n->op == '=')) {
    fail(n->line, dst->a->sid >= 0 ? "con una estructura solo se puede usar '='" : "no se puede asignar un array completo; asigna elemento a elemento");
    return Value();
  }
  if (n->op == '=') {
    *dst = coerce(std::move(rhs), ty, n->line);
    return *dst;
  }
  int bop;
  switch (n->op) {
    case 'L': bop = OP_SHL; break;
    case 'R': bop = OP_SHR; break;
    default: bop = n->op; break;
  }
  if (dst->t == V_STR && bop == '+') {
    std::string add = toStr(rhs);
    if (dst->s->rc == 1) dst->s->s += add;
    else *dst = Value::S(dst->s->s + add);
    return *dst;
  }
  Value r = binop(bop, *dst, rhs, n->line);
  if (abort_) return Value();
  *dst = coerce(std::move(r), ty, n->line);
  return *dst;
}

Value Impl::incdec(Node* n) {
  uint8_t ty;
  Value* dst = lref(n->a, ty);
  if (!dst) return Value();
  if (!dst->isNum()) { fail(n->line, "++/-- necesita un numero pero es %s", kindName(*dst)); return Value(); }
  Value old = *dst;
  int delta = (n->kind == N_PREINC || n->kind == N_POSTINC) ? 1 : -1;
  Value nv = dst->t == V_FLT ? Value::F(dst->f + delta) : Value::I(dst->i + delta);
  *dst = coerce(std::move(nv), ty, n->line);
  if (n->kind == N_POSTINC || n->kind == N_POSTDEC) return old;
  return *dst;
}

int64_t Impl::arrBytes(ArrObj* a) {
  if (a->sid >= 0) return tyBytes((uint8_t)(TY_STRUCT0 + a->sid));
  if (a->v.empty()) return 0;
  if (a->v[0].t == V_ARR) return (int64_t)a->v.size() * arrBytes(a->v[0].a);
  return (int64_t)a->v.size() * tyBytes(a->elemTy);
}

int64_t Impl::tyBytes(uint8_t ty) {
  if (tyIsStruct(ty)) {
    int64_t total = 0;
    const StructDef& d = structs[ty - TY_STRUCT0];
    for (size_t k = 0; k < d.f.size(); k++) {
      int64_t m = 1;
      for (size_t q = 0; q < d.f[k].dims.size(); q++) m *= d.f[k].dims[q];
      total += m * tyBytes(d.f[k].ty);
    }
    return total;
  }
  return ty < TY__COUNT ? kTySize[ty] : 4;
}

Value* Impl::fieldOf(const Value& base, Node* n, uint8_t& ty) {
  if (base.t != V_ARR || base.a->sid < 0) {
    const char* f = names[n->idx].c_str();
    if (base.t == V_STR) fail(n->line, "'.%s' es un metodo de String: escribe .%s()", f, f);
    else fail(n->line, "'.%s' se usa sobre algo que no es una estructura", f);
    return nullptr;
  }
  int sid = base.a->sid;
  int fi;
  if (n->op == (uint8_t)(sid + 1)) {
    fi = (int)n->ival;
  } else {
    fi = fieldIndex(sid, n->idx);
    if (fi < 0) {
      fail(n->line, "'%s' no tiene el campo '%s'", structs[sid].name.c_str(), names[n->idx].c_str());
      return nullptr;
    }
    n->op = (uint8_t)(sid + 1);
    n->ival = fi;
  }
  const StructField& f = structs[sid].f[(size_t)fi];
  ty = f.dims.empty() ? f.ty : (uint8_t)TY_ANY;
  return &base.a->v[(size_t)fi];
}

Value Impl::evalField(Node* n) {
  Value base = eval(n->a);
  if (abort_) return Value();
  uint8_t ty;
  Value* p = fieldOf(base, n, ty);
  if (!p) return Value();
  return *p;
}

Value Impl::cloneVal(const Value& v) {
  if (v.t != V_ARR) return v;
  ArrObj* o = new ArrObj;
  o->rc = 1;
  o->elemTy = v.a->elemTy;
  o->sid = v.a->sid;
  o->v.resize(v.a->v.size());
  Value out = Value::A(o);
  for (size_t k = 0; k < v.a->v.size(); k++) o->v[k] = cloneVal(v.a->v[k]);
  return out;
}

Value Impl::fieldValue(const StructField& f, Node* val, int line) {
  if (!f.dims.empty()) {
    if (val && val->kind != N_INITLIST && val->kind != N_STR) { fail(line, "un array se inicializa con { ... }"); return Value(); }
    return makeArray(f.dims, 0, f.ty, val, line);
  }
  if (tyIsStruct(f.ty) && (!val || val->kind == N_INITLIST)) return makeStruct(f.ty - TY_STRUCT0, val, line);
  if (!val) return defaultValue(f.ty);
  if (val->kind == N_INITLIST) {   // int x = {5};
    if (!val->a) return defaultValue(f.ty);
    if (val->a->next) { fail(line, "sobran valores entre { }"); return Value(); }
    val = val->a;
  }
  if (val->kind == N_DESIG) { fail(line, "'.%s =' sobra aqui", names[val->idx].c_str()); return Value(); }
  Value v = eval(val);
  if (abort_) return Value();
  return coerce(std::move(v), f.ty, line);
}

Value Impl::makeStruct(int sid, Node* init, int line) {
  ArrObj* o = new ArrObj;
  o->rc = 1;
  o->elemTy = TY_ANY;
  o->sid = (int16_t)sid;
  size_t nf = structs[sid].f.size();
  o->v.resize(nf);
  Value out = Value::A(o);
  for (size_t k = 0; k < nf; k++) {
    o->v[k] = fieldValue(structs[sid].f[k], structs[sid].f[k].def, line);
    if (abort_) return Value();
  }
  if (!init) return out;
  if (init->kind != N_INITLIST) { fail(line, "una estructura se inicializa con { ... }"); return Value(); }
  size_t pos = 0;
  for (Node* e = init->a; e; e = e->next) {
    Node* val = e;
    if (e->kind == N_DESIG) {
      int fi = fieldIndex(sid, e->idx);
      if (fi < 0) { fail(e->line, "'%s' no tiene el campo '%s'", structs[sid].name.c_str(), names[e->idx].c_str()); return Value(); }
      pos = (size_t)fi;
      val = e->a;
    }
    if (pos >= nf) { fail(line, "hay mas valores que campos en '%s'", structs[sid].name.c_str()); return Value(); }
    o->v[pos] = fieldValue(structs[sid].f[pos], val, line);
    if (abort_) return Value();
    pos++;
  }
  return out;
}

void Impl::fillValue(Value& v, uint8_t ty, int64_t b) {
  if (v.t == V_ARR) {
    ArrObj* a = v.a;
    for (size_t k = 0; k < a->v.size(); k++)
      fillValue(a->v[k], a->sid >= 0 ? structs[a->sid].f[k].ty : a->elemTy, b);
    return;
  }
  if (v.t == V_FLT) { v.f = 0; return; }
  if (v.t == V_STR) { v = Value::S(std::string()); return; }
  if (v.t == V_INT) {
    int64_t x = b ? (int64_t)((uint64_t)b * 0x0101010101010101ULL) : 0;
    v.i = (ty < TY__COUNT && tyIsInt(ty)) ? wrapInt(x, ty) : x;
  }
}

bool Impl::flatten(const Value& v, double* args, int& k, int max, int line) {
  for (size_t q = 0; q < v.a->v.size(); q++) {
    const Value& e = v.a->v[q];
    if (e.t == V_ARR && e.a->sid >= 0) {
      if (!flatten(e, args, k, max, line)) return false;
      continue;
    }
    if (!e.isNum()) { fail(line, "el campo '%s' debe ser un numero", names[structs[v.a->sid].f[q].name].c_str()); return false; }
    if (k >= max) { fail(line, "estructura demasiado grande"); return false; }
    args[k++] = num(e);
  }
  return true;
}

Value Impl::evalUnary(Node* n) {
      Value a = eval(n->a);
      if (abort_) return Value();
      if (!a.isNum()) { fail(n->line, "se esperaba un numero pero es %s", kindName(a)); return Value(); }
      if (n->kind == N_POS) { a.isChar = 0; return a; }
      if (n->kind == N_NEG) return a.t == V_FLT ? Value::F(-a.f) : Value::I((int64_t)(0 - (uint64_t)a.i));
      if (a.t == V_FLT) { fail(n->line, "el operador ~ solo funciona con enteros"); return Value(); }
      return Value::I(~a.i);
    }

Value Impl::evalIndex(Node* n) {
      Value base = eval(n->a);
      if (abort_) return Value();
      Value iv = eval(n->b);
      if (abort_) return Value();
      if (!iv.isNum()) { fail(n->line, "el indice debe ser un numero"); return Value(); }
      int64_t i = iv.t == V_INT ? iv.i : f2i(iv.f);
      if (base.t == V_ARR) {
        if (i < 0 || i >= (int64_t)base.a->v.size()) {
          fail(n->line, "indice %lld fuera del array (tamano %d)", (long long)i, (int)base.a->v.size());
          return Value();
        }
        return base.a->v[(size_t)i];
      }
      if (base.t == V_STR) {
        const std::string& s = base.s->s;
        if (i == (int64_t)s.size()) return Value::I(0, true);
        if (i < 0 || i > (int64_t)s.size()) { fail(n->line, "indice %lld fuera del texto (longitud %d)", (long long)i, (int)s.size()); return Value(); }
        return Value::I((int8_t)s[(size_t)i], true);
      }
      fail(n->line, "se usa [ ] sobre algo que no es un array");
      return Value();
    }

Value Impl::evalCast(Node* n) {
      Value v = eval(n->a);
      if (abort_) return Value();
      if (n->op == TY_STRING) return coerce(std::move(v), TY_STRING, n->line);
      if (v.t == V_STR) {
        fail(n->line, "no se puede convertir un String a numero con (tipo); usa .toInt() o .toFloat()");
        return Value();
      }
      return coerce(std::move(v), n->op, n->line);
    }

Value Impl::evalSizeof(Node* n) {
      if (!n->a) return Value::I(tyBytes(n->op));
      Node* x = n->a;
      Value v = eval(x);
      if (abort_) return Value();
      if (v.t == V_ARR) return Value::I(arrBytes(v.a));
      uint8_t ty = TY_ANY;
      if (x->kind == N_LVAR || x->kind == N_GVAR || x->kind == N_CAST) ty = x->op;
      else if (x->kind == N_INDEX) {
        Value b = eval(x->a);
        if (abort_) return Value();
        if (b.t == V_ARR) ty = b.a->elemTy;
        else if (b.t == V_STR) ty = TY_CHAR;
      }
      if (ty == TY_ANY) ty = v.t == V_FLT ? TY_DOUBLE : (v.t == V_STR ? TY_STRING : TY_I32);
      return Value::I(tyBytes(ty));
    }

Value Impl::eval(Node* n) {
  switch (n->kind) {
    case N_NUM: return Value::I(n->ival, n->op != 0);
    case N_FLT: return Value::F(n->fval);
    case N_STR: return strConsts[n->idx];
    case N_LVAR: {
      Value* v = &stack[fp + n->idx];
      if (v->t == V_REF) v = v->r;
      return *v;
    }
    case N_GVAR: return globals[n->idx];
    case N_BIN: {
      Value a = eval(n->a);
      if (abort_) return Value();
      Value b = eval(n->b);
      if (abort_) return Value();
      return binop(n->idx, a, b, n->line);
    }
    case N_AND: {
      Value a = eval(n->a);
      if (abort_) return Value();
      if (!truthy(a)) return Value::I(0);
      Value b = eval(n->b);
      if (abort_) return Value();
      return Value::I(truthy(b));
    }
    case N_OR: {
      Value a = eval(n->a);
      if (abort_) return Value();
      if (truthy(a)) return Value::I(1);
      Value b = eval(n->b);
      if (abort_) return Value();
      return Value::I(truthy(b));
    }
    case N_NOT: {
      Value a = eval(n->a);
      if (abort_) return Value();
      return Value::I(!truthy(a));
    }
    case N_NEG: case N_POS: case N_BITNOT:
      return evalUnary(n);
    case N_PREINC: case N_PREDEC: case N_POSTINC: case N_POSTDEC:
      return incdec(n);
    case N_TERN: {
      Value c = eval(n->a);
      if (abort_) return Value();
      return eval(truthy(c) ? n->b : n->c);
    }
    case N_ASSIGN: return assign(n);
    case N_INDEX: return evalIndex(n);
    case N_CALL: return callUser(n->idx, n->b, n->line);
    case N_BUILTIN: return builtin(n);
    case N_METHOD: return method(n);
    case N_CAST: return evalCast(n);
    case N_SIZEOF: return evalSizeof(n);
    case N_COMMA: {
      eval(n->a);
      if (abort_) return Value();
      return eval(n->b);
    }
    case N_INITLIST:
      fail(n->line, "{ ... } solo se puede usar al declarar un array");
      return Value();
    case N_FUNCREF:
      return Value::Fn(n->idx);
    case N_ADDR: {   // &estructura o &array: se pasa por referencia
      Value v = eval(n->a);
      if (abort_) return Value();
      if (v.t == V_ARR) return v;
      fail(n->line, "&x solo se admite con estructuras, arrays y funciones; para numeros usa parametros por referencia (int &x)");
      return Value();
    }
    case N_FIELD:
      return evalField(n);
    case N_DESIG:
      fail(n->line, "'.%s = ...' solo se puede usar dentro de { } al crear una estructura", names[n->idx].c_str());
      return Value();
    default:
      fail(n->line, "expresion no soportada");
      return Value();
  }
}

bool Impl::bindArgs(Func& f, Node* args, int newfp, int line) {
  int np = (int)f.params.size();
  Node* a = args;
  for (int k = 0; k < np; k++) {
    const Param& p = f.params[k];
    if (a) {
      if (p.isRef) {
        uint8_t ty;
        if (a->kind != N_LVAR && a->kind != N_GVAR && a->kind != N_INDEX && a->kind != N_FIELD) {
          fail(a->line, "el parametro %d de '%s' es por referencia (&): pasa una variable", k + 1, f.name.c_str());
        } else {
          Value* r = lref(a, ty);
          if (r) stack[newfp + k] = Value::Ref(r);
        }
      } else {
        Value v = eval(a);
        if (!abort_) stack[newfp + k] = coerce(std::move(v), p.ty, a->line);
      }
      a = a->next;
    } else {
      Value v = eval(p.def);
      if (!abort_) stack[newfp + k] = coerce(std::move(v), p.ty, line);
    }
    if (abort_) {
      for (int q = newfp; q < newfp + np; q++) stack[q] = Value();
      return false;
    }
  }
  return true;
}

Value Impl::finishCall(Func& f, int line) {
  Value r = std::move(retVal);
  retVal = Value();
  if (f.retTy == TY_VOID) return Value();
  if (r.t == V_VOID) return defaultValue(f.retTy);
  return coerce(std::move(r), f.retTy, line);
}

Value Impl::callUser(int fi, Node* args, int line) {
  Func& f = funcs[fi];
  if (depth >= MAX_CALL_DEPTH) { fail(line, "demasiadas llamadas anidadas (recursion infinita?)"); return Value(); }
  int newfp = sp;
  if (newfp + f.nslots + 1 > (int)stack.size()) { fail(line, "memoria de variables locales agotada (recursion?)"); return Value(); }
  sp = newfp + (int)f.params.size();  // reservar parametros
  if (!bindArgs(f, args, newfp, line)) { sp = newfp; return Value(); }
  sp = newfp + f.nslots;
  int savedFp = fp;
  fp = newfp;
  depth++;
  retVal = Value();
  exec(f.body);
  depth--;
  fp = savedFp;
  for (int q = newfp; q < newfp + f.nslots; q++) stack[q] = Value();
  sp = newfp;
  if (abort_) return Value();
  return finishCall(f, line);
}

Value Impl::makeArray(const std::vector<int>& dims, size_t level, uint8_t ty, Node* init, int line) {
  int len = dims[level];
  if (len < 0) {
    if (init && init->kind == N_INITLIST) len = (int)init->ival;
    else if (init && init->kind == N_STR) len = (int)strConsts[init->idx].s->s.size() + 1;
    else { fail(line, "falta el tamano del array"); return Value(); }
  }
  int64_t total = len;
  for (size_t k = level + 1; k < dims.size(); k++) total *= (dims[k] > 0 ? dims[k] : 1);
  if (len < 0 || total > MAX_ARRAY_ELEMS) {
    fail(line, "array demasiado grande (maximo %d elementos)", MAX_ARRAY_ELEMS);
    return Value();
  }
  ArrObj* arr = new ArrObj;
  arr->rc = 1;
  arr->elemTy = ty;
  arr->v.resize((size_t)len);
  Value out = Value::A(arr);
  if (init && init->kind == N_STR && level + 1 == dims.size()) {
    const std::string& s = strConsts[init->idx].s->s;
    for (int k = 0; k < len; k++)
      arr->v[(size_t)k] = Value::I(k < (int)s.size() ? (int8_t)s[(size_t)k] : 0, ty == TY_CHAR);
    return out;
  }
  Node* e = (init && init->kind == N_INITLIST) ? init->a : nullptr;
  for (int k = 0; k < len; k++) {
    if (level + 1 < dims.size()) {
      Node* sub = nullptr;
      if (e) {
        sub = e;
        e = e->next;
        if (sub->kind != N_INITLIST) { fail(line, "inicializador de array multidimensional mal formado"); return Value(); }
      }
      arr->v[(size_t)k] = makeArray(dims, level + 1, ty, sub, line);
    } else if (e && e->kind == N_INITLIST && tyIsStruct(ty)) {
      arr->v[(size_t)k] = makeStruct(ty - TY_STRUCT0, e, line);
      e = e->next;
    } else if (e) {
      if (e->kind == N_INITLIST) { fail(line, "sobran llaves { } en el inicializador"); return Value(); }
      Value v = eval(e);
      if (abort_) return Value();
      arr->v[(size_t)k] = coerce(std::move(v), ty, line);
      e = e->next;
    } else {
      arr->v[(size_t)k] = defaultValue(ty);
    }
    if (abort_) return Value();
  }
  if (e) { fail(line, "hay mas valores que el tamano del array"); return Value(); }
  return out;
}

void Impl::execDecl(Node* n) {
  Value* slot = (n->ival & DF_GLOBAL) ? &globals[n->idx] : &stack[fp + n->idx];
  if (n->b) {
    std::vector<int> dims;
    for (Node* d = n->b; d; d = d->next) {
      if (d->a) {
        Value v = eval(d->a);
        if (abort_) return;
        if (!v.isNum()) { fail(n->line, "el tamano del array debe ser un numero"); return; }
        int64_t sz = v.t == V_INT ? v.i : f2i(v.f);
        if (sz <= 0 || sz > MAX_ARRAY_ELEMS) { fail(n->line, "tamano de array no valido (%lld)", (long long)sz); return; }
        dims.push_back((int)sz);
      } else {
        dims.push_back(-1);
      }
    }
    Value arr = makeArray(dims, 0, n->op, n->a, n->line);
    if (abort_) return;
    *slot = std::move(arr);
    return;
  }
  if (tyIsStruct(n->op)) {
    Node* init = n->a;
    Value v = (!init || init->kind == N_INITLIST) ? makeStruct(n->op - TY_STRUCT0, init, n->line) : eval(init);
    if (abort_) return;
    *slot = (!init || init->kind == N_INITLIST) ? std::move(v) : coerce(std::move(v), n->op, n->line);
    return;
  }
  if (n->a) {
    Node* init = n->a;
    if (init->kind == N_INITLIST) {
      if (!init->a) { *slot = defaultValue(n->op); return; }
      init = init->a;
    }
    Value v = eval(init);
    if (abort_) return;
    if (n->op == TY_ANY) { *slot = std::move(v); return; }
    *slot = coerce(std::move(v), n->op, n->line);
    return;
  }
  *slot = defaultValue(n->op);
}

void Impl::dispatchEvents(int line) {
  uint32_t bits = hal.takeEvents();
  for (int b = 0; b < 32 && bits; b++) {
    uint32_t m = 1u << b;
    if (!(bits & m)) continue;
    bits &= ~m;
    int f = eventFunc[b];
    uint32_t times = hal.takeEventCount(b);
    if (f < 0 || times == 0) continue;
    if (times > 2000) times = 2000;   // no bloquear el programa si se acumulan muchas
    inEvent = true;
    for (uint32_t t = 0; t < times && !abort_; t++) callUser(f, nullptr, line);
    inEvent = false;
    if (abort_) return;
  }
}

bool Impl::sleepMs(uint32_t ms) {
  uint32_t start = hal.millis();
  for (;;) {
    if (checkStop()) return false;
    if (hal.eventBits && !inEvent && eventMask == 0 && depth < MAX_CALL_DEPTH - 12) {
      dispatchEvents(0);
      if (abort_) return false;
    }
    uint32_t el = hal.millis() - start;
    if (el >= ms) break;
    uint32_t left = ms - el;
    hal.delayMs(left > 20 ? 20 : left);
  }
  return true;
}

Flow Impl::execLoop(Node* n) {
  switch (n->kind) {
    case N_WHILE:
      for (;;) {
        Value c = eval(n->a);
        if (abort_) return F_ABORT;
        if (!truthy(c)) break;
        Flow f = exec(n->b);
        if (f == F_BREAK) break;
        if (f == F_RETURN || f == F_ABORT) return f;
      }
      return F_NORMAL;
    case N_DOWHILE:
      for (;;) {
        Flow f = exec(n->b);
        if (f == F_BREAK) break;
        if (f == F_RETURN || f == F_ABORT) return f;
        Value c = eval(n->a);
        if (abort_) return F_ABORT;
        if (!truthy(c)) break;
      }
      return F_NORMAL;
    case N_FOR: {
      if (n->a) {
        Flow f = exec(n->a);
        if (f == F_ABORT) return f;
      }
      for (;;) {
        if (n->b) {
          Value c = eval(n->b);
          if (abort_) return F_ABORT;
          if (!truthy(c)) break;
        }
        Flow f = exec(n->d);
        if (f == F_BREAK) break;
        if (f == F_RETURN || f == F_ABORT) return f;
        if (n->c) {
          eval(n->c);
          if (abort_) return F_ABORT;
        }
        if (stopReq.load(std::memory_order_relaxed)) { stopAbort(); return F_ABORT; }
      }
      return F_NORMAL;
    }
    default: return F_NORMAL;
  }
}

Flow Impl::execForeach(Node* n) {
      Value coll = eval(n->a);
      if (abort_) return F_ABORT;
      if (coll.t == V_STR) {
        std::string s = coll.s->s;
        for (size_t k = 0; k < s.size(); k++) {
          stack[fp + n->idx] = coerce(Value::I((int8_t)s[k], true), n->op, n->line);
          if (abort_) return F_ABORT;
          Flow f = exec(n->b);
          if (f == F_BREAK) break;
          if (f == F_RETURN || f == F_ABORT) return f;
        }
        return F_NORMAL;
      }
      if (coll.t != V_ARR) { fail(n->line, "for (x : ...) necesita un array o String"); return F_ABORT; }
      for (size_t k = 0; k < coll.a->v.size(); k++) {
        if (n->ival & DF_REF) stack[fp + n->idx] = Value::Ref(&coll.a->v[k]);
        else stack[fp + n->idx] = coerce(coll.a->v[k], n->op, n->line);
        if (abort_) return F_ABORT;
        Flow f = exec(n->b);
        if (f == F_BREAK) break;
        if (f == F_RETURN || f == F_ABORT) return f;
      }
      stack[fp + n->idx] = Value();
      return F_NORMAL;
    }

Flow Impl::execSwitch(Node* n) {
      Value v = eval(n->a);
      if (abort_) return F_ABORT;
      if (!v.isNum()) { fail(n->line, "switch necesita un numero o caracter"); return F_ABORT; }
      Node* start = nullptr;
      Node* def = nullptr;
      for (Node* s = n->b->a; s; s = s->next) {
        if (s->kind == N_CASE) {
          Value cv = eval(s->a);
          if (abort_) return F_ABORT;
          if (cv.isNum() && num(cv) == num(v)) { start = s; break; }
        } else if (s->kind == N_DEFAULT && !def) {
          def = s;
        }
      }
      if (!start) start = def;
      for (Node* s = start; s; s = s->next) {
        Flow f = exec(s);
        if (f == F_BREAK) return F_NORMAL;
        if (f != F_NORMAL) return f;
      }
      return F_NORMAL;
    }

Flow Impl::exec(Node* n) {
  if (((++steps) & 1023) == 0) hal.yield();
  if (stopReq.load(std::memory_order_relaxed)) { stopAbort(); return F_ABORT; }
  if (hal.eventBits && !inEvent && eventMask == 0 && depth < MAX_CALL_DEPTH - 12) {
    dispatchEvents(n->line);
    if (abort_) return F_ABORT;
  }
  switch (n->kind) {
    case N_BLOCK:
      for (Node* s = n->a; s; s = s->next) {
        Flow f = exec(s);
        if (f != F_NORMAL) return f;
      }
      return F_NORMAL;
    case N_EXPR:
      eval(n->a);
      return abort_ ? F_ABORT : F_NORMAL;
    case N_DECL:
      execDecl(n);
      return abort_ ? F_ABORT : F_NORMAL;
    case N_IF: {
      Value c = eval(n->a);
      if (abort_) return F_ABORT;
      if (truthy(c)) return exec(n->b);
      if (n->c) return exec(n->c);
      return F_NORMAL;
    }
    case N_WHILE: case N_DOWHILE: case N_FOR:
      return execLoop(n);
    case N_FOREACH: return execForeach(n);
    case N_BREAK: return F_BREAK;
    case N_CONTINUE: return F_CONTINUE;
    case N_RETURN:
      if (n->a) {
        retVal = eval(n->a);
        if (abort_) return F_ABORT;
      }
      return F_RETURN;
    case N_SWITCH: return execSwitch(n);
    case N_CASE: case N_DEFAULT: case N_EMPTY:
      return F_NORMAL;
    default:
      fail(n->line, "sentencia no soportada");
      return F_ABORT;
  }
}

// ---- printf ----
std::string Impl::formatPrintf(const std::string& fmt, Value* av, int n, int line) {
  std::string out;
  int ai = 0;
  char spec[32];
  char buf[96];
  for (size_t k = 0; k < fmt.size(); k++) {
    char c = fmt[k];
    if (c != '%') { out.push_back(c); continue; }
    if (k + 1 < fmt.size() && fmt[k + 1] == '%') { out.push_back('%'); k++; continue; }
    size_t st = k;
    k++;
    std::string flags;
    while (k < fmt.size() && strchr("-+ #0", fmt[k])) flags.push_back(fmt[k++]);
    int width = -1, prec = -1;
    if (k < fmt.size() && fmt[k] == '*') { width = ai < n && av[ai].isNum() ? (int)num(av[ai]) : 0; ai++; k++; }
    else { int w = 0; bool any = false; while (k < fmt.size() && isdigit((unsigned char)fmt[k])) { w = w * 10 + (fmt[k++] - '0'); any = true; } if (any) width = w; }
    if (k < fmt.size() && fmt[k] == '.') {
      k++;
      if (k < fmt.size() && fmt[k] == '*') { prec = ai < n && av[ai].isNum() ? (int)num(av[ai]) : 0; ai++; k++; }
      else { int p = 0; while (k < fmt.size() && isdigit((unsigned char)fmt[k])) p = p * 10 + (fmt[k++] - '0'); prec = p; }
    }
    while (k < fmt.size() && strchr("hlLqjzt", fmt[k])) k++;
    if (k >= fmt.size()) { out.append(fmt, st, std::string::npos); break; }
    char conv = fmt[k];
    if (width > 60) width = 60;
    if (prec > 30) prec = 30;
    std::string base = "%" + flags;
    if (width >= 0) base += std::to_string(width);
    if (prec >= 0) base += "." + std::to_string(prec);
    if (ai >= n) { fail(line, "printf: faltan valores para el formato"); return out; }
    const Value& v = av[ai++];
    switch (conv) {
      case 'd': case 'i': {
        if (!v.isNum()) { fail(line, "printf: %%%c necesita un numero", conv); return out; }
        int64_t x = v.t == V_INT ? v.i : f2i(v.f);
        snprintf(spec, sizeof spec, "%slld", base.c_str());
        snprintf(buf, sizeof buf, spec, (long long)x);
        out += buf;
        break;
      }
      case 'u': case 'x': case 'X': case 'o': {
        if (!v.isNum()) { fail(line, "printf: %%%c necesita un numero", conv); return out; }
        int64_t x = v.t == V_INT ? v.i : f2i(v.f);
        uint64_t u = (x < 0 && x >= INT32_MIN) ? (uint64_t)(uint32_t)x : (uint64_t)x;
        snprintf(spec, sizeof spec, "%sll%c", base.c_str(), conv);
        snprintf(buf, sizeof buf, spec, (unsigned long long)u);
        out += buf;
        break;
      }
      case 'c': {
        int x = v.isNum() ? (int)num(v) : (v.t == V_STR && !v.s->s.empty() ? v.s->s[0] : '?');
        snprintf(spec, sizeof spec, "%sc", base.c_str());
        snprintf(buf, sizeof buf, spec, x);
        out += buf;
        break;
      }
      case 's': {
        std::string s = toStr(v);
        if (prec >= 0 && (int)s.size() > prec) s.resize((size_t)prec);
        if (width > (int)s.size()) {
          std::string pad((size_t)(width - (int)s.size()), ' ');
          s = flags.find('-') != std::string::npos ? s + pad : pad + s;
        }
        out += s;
        break;
      }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
        if (!v.isNum()) { fail(line, "printf: %%%c necesita un numero", conv); return out; }
        double x = num(v);
        if (fabs(x) > 1e30 && (conv == 'f' || conv == 'F')) { out += "ovf"; break; }
        snprintf(spec, sizeof spec, "%s%c", base.c_str(), conv);
        snprintf(buf, sizeof buf, spec, x);
        out += buf;
        break;
      }
      default:
        fail(line, "printf: formato %%%c no soportado", conv);
        return out;
    }
  }
  return out;
}

static inline int64_t asInt(const Value& v) { return v.t == V_INT ? v.i : (v.t == V_FLT ? Impl::f2i(v.f) : 0); }

Value Impl::builtin(Node* n) {
  if (n->idx >= B__COUNT) return nativeCall(n);
  int id = n->idx;
  int argc = (int)n->ival;
  int line = n->line;
  // funciones que modifican su primer argumento
  if (id == B_BITSET || id == B_BITCLEAR || id == B_BITWRITE) {
    Value bv = eval(n->b->next);
    if (abort_) return Value();
    Value wv;
    if (id == B_BITWRITE) { wv = eval(n->b->next->next); if (abort_) return Value(); }
    uint8_t ty;
    Value* dst = lref(n->b, ty);
    if (!dst) return Value();
    if (dst->t != V_INT || !bv.isNum()) { fail(line, "%s() necesita enteros", kBuiltins[id].name); return Value(); }
    int64_t b = asInt(bv);
    int64_t x = dst->i;
    bool set = id == B_BITSET || (id == B_BITWRITE && truthy(wv));
    if (b >= 0 && b < 64) x = set ? (x | ((int64_t)1 << b)) : (x & ~((int64_t)1 << b));
    *dst = coerce(Value::I(x), ty, line);
    return *dst;
  }
  Value av[MAX_ARGS];
  {
    Node* a = n->b;
    for (int k = 0; k < argc && a; k++, a = a->next) {
      av[k] = eval(a);
      if (abort_) return Value();
      if (av[k].t == V_VOID) { fail(line, "el argumento %d de %s() no tiene valor (es void)", k + 1, kBuiltins[id].name); return Value(); }
    }
  }
  // comprobar argumentos numericos para la mayoria
  bool needNums = !(id == B_S_PRINT || id == B_S_PRINTLN || id == B_S_PRINTF || id == B_S_WRITE ||
                    id == B_F || id == B_STRING2 || id == B_S_READSTRINGUNTIL || id == B_STRLEN ||
                    id == B_MIN || id == B_MAX);
  if (needNums) {
    for (int k = 0; k < argc; k++) {
      if (!av[k].isNum()) {
        fail(line, "el argumento %d de %s() debe ser un numero pero es %s", k + 1, kBuiltins[id].name, kindName(av[k]));
        return Value();
      }
    }
  }
  // comprobar que los pines se pueden usar
  {
    int pinArgs = 0;  // bits: argumentos que son pines; outMask: los que son salida
    int outMask = 0;
    switch (id) {
      case B_PINMODE: {
        pinArgs = 1;
        int m = (int)asInt(av[1]);
        if (m == MC_OUTPUT || m == MC_OUTPUT_OPEN_DRAIN) outMask = 1;
        break;
      }
      case B_DWRITE: case B_AWRITE: case B_DACWRITE: case B_TONE: case B_NOTONE: pinArgs = 1; outMask = 1; break;
      case B_DREAD: case B_AREAD: case B_TOUCHREAD: case B_PULSEIN: pinArgs = 1; break;
      case B_SHIFTOUT: pinArgs = 3; outMask = 3; break;
      case B_SHIFTIN: pinArgs = 3; outMask = 2; break;
      default: break;
    }
    for (int k = 0; k < 2; k++) {
      if (!(pinArgs & (1 << k))) continue;
      const char* e = hal.pinError((int)asInt(av[k]), (outMask & (1 << k)) != 0);
      if (e) { fail(line, "%s", e); return Value(); }
    }
  }
  switch (id) {
    case B_PINMODE: hal.pinMode((int)asInt(av[0]), (int)asInt(av[1])); return Value();
    case B_DWRITE: hal.digitalWrite((int)asInt(av[0]), truthy(av[1]) ? 1 : 0); return Value();
    case B_DREAD: return Value::I(hal.digitalRead((int)asInt(av[0])));
    case B_AREAD: return Value::I(hal.analogRead((int)asInt(av[0])));
    case B_AWRITE: hal.analogWrite((int)asInt(av[0]), (int)asInt(av[1])); return Value();
    case B_AREADRES: hal.analogReadResolution((int)asInt(av[0])); return Value();
    case B_DACWRITE: hal.dacWrite((int)asInt(av[0]), (int)asInt(av[1])); return Value();
    case B_TOUCHREAD: return Value::I(hal.touchRead((int)asInt(av[0])));
    case B_TONE: hal.tone((int)asInt(av[0]), (unsigned)asInt(av[1]), argc > 2 ? (unsigned long)asInt(av[2]) : 0); return Value();
    case B_NOTONE: hal.noTone((int)asInt(av[0])); return Value();
    case B_PULSEIN: return Value::I((int64_t)hal.pulseIn((int)asInt(av[0]), (int)asInt(av[1]), argc > 2 ? (unsigned long)asInt(av[2]) : 1000000UL));
    case B_SHIFTOUT: {
      int dp = (int)asInt(av[0]), cp = (int)asInt(av[1]), order = (int)asInt(av[2]);
      int val = (int)asInt(av[3]) & 0xFF;
      for (int k = 0; k < 8; k++) {
        int bitv = order == 0 ? (val >> k) & 1 : (val >> (7 - k)) & 1;
        hal.digitalWrite(dp, bitv);
        hal.digitalWrite(cp, 1);
        hal.digitalWrite(cp, 0);
      }
      return Value();
    }
    case B_SHIFTIN: {
      int dp = (int)asInt(av[0]), cp = (int)asInt(av[1]), order = (int)asInt(av[2]);
      int val = 0;
      for (int k = 0; k < 8; k++) {
        hal.digitalWrite(cp, 1);
        int bitv = hal.digitalRead(dp) ? 1 : 0;
        if (order == 0) val |= bitv << k; else val |= bitv << (7 - k);
        hal.digitalWrite(cp, 0);
      }
      return Value::I(val);
    }
    case B_DELAY: {
      int64_t ms = asInt(av[0]);
      if (ms > 0) sleepMs((uint32_t)(ms > 0x7FFFFFFF ? 0x7FFFFFFF : ms));
      else hal.yield();
      return Value();
    }
    case B_DELAYUS: {
      int64_t us = asInt(av[0]);
      if (us <= 0) return Value();
      if (us > 20000) sleepMs((uint32_t)(us / 1000));
      else hal.delayUs((uint32_t)us);
      return Value();
    }
    case B_MILLIS: return Value::I(hal.millis());
    case B_MICROS: return Value::I(hal.micros());
    case B_RANDOM: {
      int64_t lo = 0, hi;
      if (argc == 1) hi = asInt(av[0]);
      else { lo = asInt(av[0]); hi = asInt(av[1]); }
      if (hi <= lo) return Value::I(lo);
      return Value::I(lo + (int64_t)(nextRand() % (uint64_t)(hi - lo)));
    }
    case B_RANDOMSEED: rng = (uint32_t)asInt(av[0]) ^ 0x9E3779B9u; if (!rng) rng = 1; nextRand(); return Value();
    case B_MAP: {
      bool allInt = true;
      for (int k = 0; k < 5; k++) if (av[k].t != V_INT) allInt = false;
      if (allInt) {
        int64_t x = av[0].i, a = av[1].i, b = av[2].i, c = av[3].i, d = av[4].i;
        if (a == b) { fail(line, "map(): el rango de entrada es vacio"); return Value(); }
        return Value::I((x - a) * (d - c) / (b - a) + c);
      }
      double x = num(av[0]), a = num(av[1]), b = num(av[2]), c = num(av[3]), d = num(av[4]);
      if (a == b) { fail(line, "map(): el rango de entrada es vacio"); return Value(); }
      return Value::F((x - a) * (d - c) / (b - a) + c);
    }
    case B_CONSTRAIN: {
      if (av[0].t == V_INT && av[1].t == V_INT && av[2].t == V_INT) {
        int64_t x = av[0].i;
        if (x < av[1].i) x = av[1].i;
        if (x > av[2].i) x = av[2].i;
        return Value::I(x);
      }
      double x = num(av[0]);
      if (x < num(av[1])) x = num(av[1]);
      if (x > num(av[2])) x = num(av[2]);
      return Value::F(x);
    }
    case B_MIN: case B_MAX: {
      if (!av[0].isNum() || !av[1].isNum()) { fail(line, "%s() necesita numeros", kBuiltins[id].name); return Value(); }
      bool less = num(av[0]) < num(av[1]);
      bool pickFirst = id == B_MIN ? less : !less;
      if (av[0].t == V_INT && av[1].t == V_INT) return Value::I(pickFirst ? av[0].i : av[1].i);
      return Value::F(pickFirst ? num(av[0]) : num(av[1]));
    }
    case B_ABS: if (av[0].t == V_INT) return Value::I(av[0].i < 0 ? -av[0].i : av[0].i); return Value::F(fabs(av[0].f));
    case B_SQ: if (av[0].t == V_INT) return Value::I(av[0].i * av[0].i); return Value::F(av[0].f * av[0].f);
    case B_POW: return Value::F(pow(num(av[0]), num(av[1])));
    case B_SQRT: return Value::F(sqrt(num(av[0])));
    case B_SIN: return Value::F(sin(num(av[0])));
    case B_COS: return Value::F(cos(num(av[0])));
    case B_TAN: return Value::F(tan(num(av[0])));
    case B_ASIN: return Value::F(asin(num(av[0])));
    case B_ACOS: return Value::F(acos(num(av[0])));
    case B_ATAN: return Value::F(atan(num(av[0])));
    case B_ATAN2: return Value::F(atan2(num(av[0]), num(av[1])));
    case B_EXP: return Value::F(exp(num(av[0])));
    case B_LOG: return Value::F(log(num(av[0])));
    case B_LOG10: return Value::F(log10(num(av[0])));
    case B_FLOOR: return Value::F(floor(num(av[0])));
    case B_CEIL: return Value::F(ceil(num(av[0])));
    case B_ROUND: return Value::F(round(num(av[0])));
    case B_FABS: return Value::F(fabs(num(av[0])));
    case B_FMOD: return Value::F(fmod(num(av[0]), num(av[1])));
    case B_RADIANS: return Value::F(num(av[0]) * 0.017453292519943295769);
    case B_DEGREES: return Value::F(num(av[0]) * 57.295779513082320876);
    case B_ISNAN: return Value::I(av[0].t == V_FLT && av[0].f != av[0].f);
    case B_ISINF: return Value::I(av[0].t == V_FLT && isinf(av[0].f));
    case B_BITREAD: { int64_t b = asInt(av[1]); return Value::I((b >= 0 && b < 64) ? ((asInt(av[0]) >> b) & 1) : 0); }
    case B_BIT: { int64_t b = asInt(av[0]); return Value::I((b >= 0 && b < 64) ? ((int64_t)1 << b) : 0); }
    case B_LOWBYTE: return Value::I(asInt(av[0]) & 0xFF);
    case B_HIGHBYTE: return Value::I((asInt(av[0]) >> 8) & 0xFF);
    case B_ISDIGIT: return Value::I(isdigit((int)(asInt(av[0]) & 0xFF)) != 0);
    case B_ISALPHA: return Value::I(isalpha((int)(asInt(av[0]) & 0xFF)) != 0);
    case B_ISALNUM: return Value::I(isalnum((int)(asInt(av[0]) & 0xFF)) != 0);
    case B_ISSPACE: return Value::I(isspace((int)(asInt(av[0]) & 0xFF)) != 0);
    case B_ISUPPER: return Value::I(isupper((int)(asInt(av[0]) & 0xFF)) != 0);
    case B_ISLOWER: return Value::I(islower((int)(asInt(av[0]) & 0xFF)) != 0);
    case B_TOUPPER: return Value::I(toupper((int)(asInt(av[0]) & 0xFF)), true);
    case B_TOLOWER: return Value::I(tolower((int)(asInt(av[0]) & 0xFF)), true);
    case B_F: return av[0];
    case B_YIELD: hal.yield(); return Value();
    case B_S_BEGIN: case B_S_FLUSH: case B_S_SETTIMEOUT: return Value();
    case B_S_PRINT: case B_S_PRINTLN: {
      std::string s;
      if (argc >= 1) {
        int fmt = -1;
        if (argc == 2) {
          if (!av[1].isNum()) { fail(line, "el segundo argumento de print debe ser DEC, HEX, BIN o el numero de decimales"); return Value(); }
          fmt = (int)asInt(av[1]);
          if (av[0].t == V_INT && av[0].isChar) av[0].isChar = 0;
        }
        if (av[0].t == V_ARR) { fail(line, "no se puede imprimir un array entero; imprime sus elementos"); return Value(); }
        s = toStr(av[0], fmt);
      }
      if (id == B_S_PRINTLN) s += "\r\n";
      out(s);
      return Value::I((int64_t)s.size());
    }
    case B_S_PRINTF: {
      if (av[0].t != V_STR) { fail(line, "Serial.printf necesita un texto de formato"); return Value(); }
      std::string s = formatPrintf(av[0].s->s, av + 1, argc - 1, line);
      if (abort_) return Value();
      out(s);
      return Value::I((int64_t)s.size());
    }
    case B_S_WRITE: {
      if (av[0].t == V_STR) { out(av[0].s->s); return Value::I((int64_t)av[0].s->s.size()); }
      if (!av[0].isNum()) { fail(line, "Serial.write necesita un numero o un texto"); return Value(); }
      char c = (char)(asInt(av[0]) & 0xFF);
      hal.write(&c, 1);
      return Value::I(1);
    }
    case B_S_AVAILABLE: return Value::I(hal.available());
    case B_S_READ: return Value::I(hal.read());
    case B_S_PEEK: return Value::I(hal.peek());
    case B_S_READSTRING: case B_S_READSTRINGUNTIL: {
      std::string s;
      int term = -1;
      if (id == B_S_READSTRINGUNTIL) {
        if (av[0].isNum()) term = (int)(asInt(av[0]) & 0xFF);
        else if (av[0].t == V_STR && !av[0].s->s.empty()) term = (unsigned char)av[0].s->s[0];
      }
      uint32_t t0 = hal.millis();
      for (;;) {
        if (hal.available() > 0) {
          int c = hal.read();
          if (c < 0) break;
          if (c == term) break;
          s.push_back((char)c);
          t0 = hal.millis();
          continue;
        }
        if (hal.millis() - t0 >= 50) break;  // espera corta a que llegue mas
        if (checkStop()) return Value();
        hal.delayMs(2);
      }
      return Value::S(std::move(s));
    }
    case B_S_PARSEINT: case B_S_PARSEFLOAT: {
      std::string s;
      uint32_t t0 = hal.millis();
      bool started = false, dot = false;
      for (;;) {
        int c = hal.available() > 0 ? hal.peek() : -1;
        if (c < 0) {
          if (hal.millis() - t0 >= 1000) break;
          if (checkStop()) return Value();
          hal.delayMs(2);
          continue;
        }
        bool ok = isdigit(c) || (c == '-' && !started) || (id == B_S_PARSEFLOAT && c == '.' && !dot);
        if (ok) { started = true; if (c == '.') dot = true; s.push_back((char)c); hal.read(); continue; }
        if (started) break;
        hal.read();
      }
      if (id == B_S_PARSEINT) return Value::I(strtoll(s.c_str(), nullptr, 10));
      return Value::F(strtod(s.c_str(), nullptr));
    }
    case B_STRING2: {
      if (!av[1].isNum()) { fail(line, "String(valor, formato): el formato debe ser un numero"); return Value(); }
      Value v = av[0];
      if (v.t == V_INT) v.isChar = 0;
      return Value::S(toStr(v, (int)asInt(av[1])));
    }
    case B_STRLEN:
      if (av[0].t != V_STR) { fail(line, "strlen() necesita un texto"); return Value(); }
      return Value::I((int64_t)av[0].s->s.size());
    default:
      fail(line, "funcion integrada no implementada");
      return Value();
  }
}

// ---- funciones del ESP32 ----
Value Impl::nativeCall(Node* n) {
  const NativeDef& d = kNatives[n->idx - B__COUNT];
  int line = n->line;
  if (d.flags & XF_NOEVAL) {  // secciones criticas: no se evaluan los argumentos
    if (d.nf == XS_NOINT || d.nf == XS_CRIT_ENTER) eventMask++;
    else if (eventMask > 0) eventMask--;
    return Value();
  }
  Node* first = n->b;
  Node* a = (d.flags & XF_SELF) ? (first ? first->next : nullptr) : first;
  Value av[MAX_ARGS];
  int na = 0;
  for (; a && na < MAX_ARGS; a = a->next) {
    av[na] = eval(a);
    if (abort_) return Value();
    na++;
  }
  // funciones que resuelve el interprete
  switch (d.nf) {
    case XS_STR: return Value::S(hal.nativeStr(d.aux));
    case XS_IDENT: return Value::I(asInt(av[0]));
    case XS_BIT: return Value::I((int64_t)1 << (asInt(av[0]) & 63));
    case XS_TICKS: return Value::I(hal.millis());
    case XS_NOOP: return Value();
    case XS_DELAYUS: hal.delayUs((uint32_t)asInt(av[0])); return Value();
    case XS_VTASKDELAY: {
      int64_t t = asInt(av[0]);
      if (t <= 0) { hal.yield(); return Value(); }
      sleepMs(t > 0xFFFFFFFFLL ? 0xFFFFFFFFu : (uint32_t)t);
      return Value();
    }
    case XS_ESPCHECK: case XS_ESPCHECK_NA: {
      int64_t code = av[0].isNum() ? asInt(av[0]) : 0;
      if (code != 0) {
        if (d.nf == XS_ESPCHECK) {
          fail(line, "ESP_ERROR_CHECK: la funcion fallo con %s (0x%llx)", espErrName(code), (long long)code);
          return Value();
        }
        out(std::string("ESP_ERROR_CHECK_WITHOUT_ABORT: ") + espErrName(code) + "\r\n");
      }
      return Value::I(code);
    }
    case XS_ERRNAME: {
      std::string nm = espErrName(asInt(av[0]));
      size_t sp = nm.find(' ');
      return Value::S(sp == std::string::npos ? nm : nm.substr(0, sp));
    }
    case XS_PRINTF: case XS_LOG: {
      int fa = d.nf == XS_LOG ? 1 : 0;
      if (av[fa].t != V_STR) { fail(line, "%s necesita un texto de formato", d.name); return Value(); }
      std::string txt = formatPrintf(av[fa].s->s, av + fa + 1, na - fa - 1, line);
      if (abort_) return Value();
      if (d.nf == XS_LOG) {
        char head[64];
        snprintf(head, sizeof head, "%c (%lu) %s: ", (char)d.aux, (unsigned long)hal.millis(), toStr(av[0]).c_str());
        txt = head + txt + "\r\n";
      }
      out(txt);
      return Value::I((int64_t)txt.size());
    }
    case XS_MEMSET:
      if (av[0].t != V_ARR) { fail(line, "memset() solo funciona con estructuras y arrays: memset(&x, 0, sizeof(x))"); return Value(); }
      fillValue(av[0], av[0].a->elemTy, asInt(av[1]) & 0xFF);
      return Value();
    case XS_PIN2INT: return av[0];
    case XS_FMIN: case XS_FMAX: case XS_HYPOT: case XS_TRUNC: case XS_CBRT: {
      for (int k = 0; k < na; k++)
        if (!av[k].isNum()) { fail(line, "%s() necesita numeros", d.name); return Value(); }
      double x = num(av[0]), y = na > 1 ? num(av[1]) : 0;
      if (d.nf == XS_FMIN) return Value::F(x < y ? x : y);
      if (d.nf == XS_FMAX) return Value::F(x > y ? x : y);
      if (d.nf == XS_HYPOT) return Value::F(sqrt(x * x + y * y));
      if (d.nf == XS_TRUNC) return Value::F(x < 0 ? ceil(x) : floor(x));
      return Value::F(cbrt(x));
    }
    case XS_SPISETTINGS: {
      int64_t clk = 1000000, order = 1, mode = 0;
      if (na >= 1) clk = asInt(av[0]);
      if (na >= 2) order = asInt(av[1]);
      if (na >= 3) mode = asInt(av[2]);
      Value v = Value::I((clk & 0xFFFFFFFFLL) | ((order & 0xFF) << 32) | ((mode & 0xFF) << 40));
      return v;
    }
    case XS_WIRE_WRITE: {
      double args[3];
      args[0] = n->op;
      int64_t count = 0;
      std::vector<int> bytes;
      if (av[0].t == V_STR) {
        for (size_t k = 0; k < av[0].s->s.size(); k++) bytes.push_back((unsigned char)av[0].s->s[k]);
      } else if (av[0].t == V_ARR) {
        int64_t len = na > 1 ? asInt(av[1]) : (int64_t)av[0].a->v.size();
        for (int64_t k = 0; k < len && k < (int64_t)av[0].a->v.size(); k++) bytes.push_back((int)(asInt(av[0].a->v[(size_t)k]) & 0xFF));
      } else if (av[0].isNum()) {
        bytes.push_back((int)(asInt(av[0]) & 0xFF));
      } else {
        fail(line, "Wire.write() necesita un numero, un texto o un array");
        return Value();
      }
      for (size_t k = 0; k < bytes.size(); k++) {
        args[1] = bytes[k];
        hal.nativeError.clear();
        count += (int64_t)hal.native(NF_WIRE_WRITE, args, 2);
        if (!hal.nativeError.empty()) { fail(line, "Wire.write(): %s", hal.nativeError.c_str()); return Value(); }
      }
      return Value::I(count);
    }
    case XS_SPI_TRANSFER: {
      double arg[1];
      if (av[0].t == V_ARR) {
        ArrObj* arr = av[0].a;
        int64_t len = na > 1 ? asInt(av[1]) : (int64_t)arr->v.size();
        for (int64_t k = 0; k < len && k < (int64_t)arr->v.size(); k++) {
          arg[0] = (double)(asInt(arr->v[(size_t)k]) & 0xFF);
          hal.nativeError.clear();
          double r = hal.native(NF_SPI_TRANSFER, arg, 1);
          if (!hal.nativeError.empty()) { fail(line, "SPI.transfer(): %s", hal.nativeError.c_str()); return Value(); }
          arr->v[(size_t)k] = coerce(Value::I((int64_t)r), arr->elemTy, line);
        }
        return Value();
      }
      if (!av[0].isNum()) { fail(line, "SPI.transfer() necesita un numero o un array"); return Value(); }
      arg[0] = (double)(asInt(av[0]) & 0xFF);
      hal.nativeError.clear();
      double r = hal.native(NF_SPI_TRANSFER, arg, 1);
      if (!hal.nativeError.empty()) { fail(line, "SPI.transfer(): %s", hal.nativeError.c_str()); return Value(); }
      return Value::I((int64_t)r);
    }
    case XS_ATTACH_INT: case XS_TIMER_ATTACH: {
      if (av[1].t != V_FUNC) {
        fail(line, "%s(): el segundo argumento debe ser el nombre de una funcion", d.name);
        return Value();
      }
      int fi = (int)av[1].i;
      if (!funcs[fi].params.empty()) {
        fail(line, "la funcion '%s' de la interrupcion no puede tener parametros", funcs[fi].name.c_str());
        return Value();
      }
      if (!av[0].isNum()) { fail(line, "%s(): el primer argumento debe ser un numero", d.name); return Value(); }
      double args[2];
      args[0] = num(av[0]);
      int nfn;
      int cnt;
      if (d.nf == XS_ATTACH_INT) {
        const char* pe = hal.pinError((int)args[0], false);
        if (pe) { fail(line, "%s", pe); return Value(); }
        if (!av[2].isNum()) { fail(line, "attachInterrupt(): el modo debe ser RISING, FALLING o CHANGE"); return Value(); }
        args[1] = num(av[2]);
        nfn = NF_PIN_EVENT_ATTACH;
        cnt = 2;
      } else {
        nfn = NF_TIMER_ATTACH;
        cnt = 1;
      }
      hal.nativeError.clear();
      int slot = (int)hal.native(nfn, args, cnt);
      if (!hal.nativeError.empty()) { fail(line, "%s(): %s", d.name, hal.nativeError.c_str()); return Value(); }
      if (slot >= 0 && slot < 32) eventFunc[slot] = fi;
      return Value();
    }
    case XS_DETACH_INT: case XS_TIMER_DETACH: {
      if (!av[0].isNum()) { fail(line, "%s() necesita un numero", d.name); return Value(); }
      double args[1] = {num(av[0])};
      hal.nativeError.clear();
      int slot = (int)hal.native(d.nf == XS_DETACH_INT ? NF_PIN_EVENT_DETACH : NF_TIMER_DETACH, args, 1);
      if (!hal.nativeError.empty()) { fail(line, "%s(): %s", d.name, hal.nativeError.c_str()); return Value(); }
      if (slot >= 0 && slot < 32) eventFunc[slot] = -1;
      return Value();
    }
    case XS_TIMER_BEGIN: {
      for (int k = 0; k < na; k++)
        if (!av[k].isNum()) { fail(line, "timerBegin() necesita numeros"); return Value(); }
      double freq;
      if (na == 1) freq = num(av[0]);                      // core 3.x: timerBegin(frecuencia)
      else if (na == 3 && num(av[1]) > 0) freq = 80000000.0 / num(av[1]);  // 2.x: timerBegin(n, divisor, subir)
      else { fail(line, "usa timerBegin(frecuencia) o timerBegin(numero, divisor, true)"); return Value(); }
      double args[1] = {freq};
      hal.nativeError.clear();
      double h = hal.native(NF_TIMER_BEGIN, args, 1);
      if (!hal.nativeError.empty()) { fail(line, "timerBegin(): %s", hal.nativeError.c_str()); return Value(); }
      return Value::I((int64_t)h);
    }
    case XS_FREQ_READ: {
      if (!av[0].isNum() || (na > 1 && !av[1].isNum())) { fail(line, "%s() necesita numeros", d.name); return Value(); }
      int pin = (int)asInt(av[0]);
      int64_t gate = na > 1 ? asInt(av[1]) : 1000;
      if (gate < 10) gate = 10;
      const char* pe = hal.pinError(pin, false);
      if (pe) { fail(line, "%s", pe); return Value(); }
      double args[2] = {(double)pin, 1.0};
      hal.nativeError.clear();
      bool started = hal.native(NF_CNT_START, args, 2) != 0;
      if (!hal.nativeError.empty()) { fail(line, "%s(): %s", d.name, hal.nativeError.c_str()); return Value(); }
      double c0 = hal.native(NF_CNT_READ, args, 1);
      uint32_t t0 = hal.micros();
      if (!sleepMs((uint32_t)gate)) return Value();
      double c1 = hal.native(NF_CNT_READ, args, 1);
      uint32_t t1 = hal.micros();
      if (started) hal.native(NF_CNT_STOP, args, 1);
      double us = (double)(uint32_t)(t1 - t0);
      return Value::F(us > 0 ? (c1 - c0) * 1e6 / us : 0.0);
    }
    default:
      break;
  }
  // llamada nativa normal: argumentos numericos (las estructuras se pasan campo a campo)
  double args[MAX_ARGS + 24];
  int k = 0;
  if (d.flags & XF_BUS) args[k++] = n->op;
  Value* self = nullptr;
  if (d.flags & XF_SELF) {
    uint8_t ty;
    self = lref(first, ty);
    if (!self) return Value();
    int64_t h = self->t == V_INT ? self->i : 0;
    if (h == 0 && d.nf != NF_SERVO_ATTACH && d.nf != NF_SERVO_ATTACHED) {
      fail(line, "el servo no esta conectado: usa primero .attach(pin)");
      return Value();
    }
    args[k++] = (double)h;
  }
  for (int q = 0; q < na; q++) {
    if ((d.flags & XF_STRUCT) && av[q].t == V_ARR && av[q].a->sid >= 0) {
      if (av[q].a->sid != d.aux) {
        fail(line, "%s() necesita un %s y le pasas un %s", d.name, structs[d.aux].name.c_str(), structs[av[q].a->sid].name.c_str());
        return Value();
      }
      if (!flatten(av[q], args, k, MAX_ARGS + 24, line)) return Value();
      continue;
    }
    if (d.flags & XF_STRUCT) {
      fail(line, "%s() necesita &variable de tipo %s", d.name, structs[d.aux].name.c_str());
      return Value();
    }
    if (!av[q].isNum()) {
      fail(line, "el argumento %d de %s() debe ser un numero pero es %s", q + 1, d.name, kindName(av[q]));
      return Value();
    }
    args[k++] = num(av[q]);
  }
  if (d.pinArg >= 0 && d.pinArg < na) {
    const char* pe = hal.pinError((int)asInt(av[d.pinArg]), d.pinOut);
    if (pe) { fail(line, "%s", pe); return Value(); }
  }
  hal.nativeError.clear();
  double r = hal.native(d.nf, args, k);
  if (!hal.nativeError.empty()) {
    fail(line, "%s(): %s", d.name, hal.nativeError.c_str());
    return Value();
  }
  if (d.nf == NF_SERVO_ATTACH && self) *self = Value::I((int64_t)r);
  switch (d.ret) {
    case 'b': return Value::I(r != 0);
    case 'i': return Value::I((int64_t)r);
    case 'f': return Value::F(r);
    default: return Value();
  }
}

static std::string& mutStr(Value* v) {
  if (v->s->rc > 1) {
    StrObj* c = new StrObj;
    c->rc = 1;
    c->s = v->s->s;
    v->s->rc--;
    v->s = c;
  }
  return v->s->s;
}

Value Impl::method(Node* n) {
  int id = n->idx;
  int argc = (int)n->ival;
  int line = n->line;
  Value av[4];
  if (argc > 3) { fail(line, "demasiados argumentos para .%s()", kMethods[id]); return Value(); }
  {
    Node* a = n->b;
    for (int k = 0; k < argc && a; k++, a = a->next) {
      av[k] = eval(a);
      if (abort_) return Value();
    }
  }
  Value tmp;
  Value* obj;
  if (id >= M_CONCAT) {
    uint8_t ty;
    obj = lref(n->a, ty);
    if (!obj) return Value();
  } else {
    tmp = eval(n->a);
    if (abort_) return Value();
    obj = &tmp;
  }
  if (obj->t != V_STR) { fail(line, ".%s() solo funciona con variables String", kMethods[id]); return Value(); }
  const std::string& s = obj->s->s;
  int sl = (int)s.size();
  auto argStr = [&](int k) -> std::string { return toStr(av[k]); };
  auto argInt = [&](int k) -> int64_t { return asInt(av[k]); };
  auto need = [&](int lo, int hi) -> bool {
    if (argc < lo || argc > hi) {
      fail(line, ".%s() necesita %d argumento(s)", kMethods[id], lo);
      return false;
    }
    return true;
  };
  switch (id) {
    case M_LENGTH: if (!need(0, 0)) return Value(); return Value::I(sl);
    case M_ISEMPTY: if (!need(0, 0)) return Value(); return Value::I(sl == 0);
    case M_CSTR: if (!need(0, 0)) return Value(); return *obj;
    case M_CHARAT: {
      if (!need(1, 1)) return Value();
      int64_t i = argInt(0);
      if (i < 0 || i >= sl) return Value::I(0, true);
      return Value::I((int8_t)s[(size_t)i], true);
    }
    case M_SUBSTRING: {
      if (!need(1, 2)) return Value();
      int64_t a = argInt(0), b = argc > 1 ? argInt(1) : sl;
      if (a > b) { int64_t t = a; a = b; b = t; }
      if (a < 0) a = 0;
      if (b > sl) b = sl;
      if (a >= sl) return Value::S(std::string());
      return Value::S(s.substr((size_t)a, (size_t)(b - a)));
    }
    case M_INDEXOF: case M_LASTINDEXOF: {
      if (!need(1, 2)) return Value();
      std::string what = (av[0].t == V_INT) ? std::string(1, (char)av[0].i) : argStr(0);
      size_t r;
      if (id == M_INDEXOF) {
        size_t from = argc > 1 ? (size_t)(argInt(1) < 0 ? 0 : argInt(1)) : 0;
        r = s.find(what, from);
      } else {
        size_t from = argc > 1 ? (size_t)(argInt(1) < 0 ? 0 : argInt(1)) : std::string::npos;
        r = s.rfind(what, from);
      }
      return Value::I(r == std::string::npos ? -1 : (int64_t)r);
    }
    case M_TOINT: if (!need(0, 0)) return Value(); return Value::I(strtol(s.c_str(), nullptr, 10));
    case M_TOFLOAT: if (!need(0, 0)) return Value(); return Value::F((double)(float)strtod(s.c_str(), nullptr));
    case M_TODOUBLE: if (!need(0, 0)) return Value(); return Value::F(strtod(s.c_str(), nullptr));
    case M_EQUALS: if (!need(1, 1)) return Value(); return Value::I(s == argStr(0));
    case M_EQUALSIC: {
      if (!need(1, 1)) return Value();
      std::string o = argStr(0);
      if (o.size() != s.size()) return Value::I(0);
      for (size_t k = 0; k < s.size(); k++)
        if (tolower((unsigned char)s[k]) != tolower((unsigned char)o[k])) return Value::I(0);
      return Value::I(1);
    }
    case M_STARTSWITH: {
      if (!need(1, 1)) return Value();
      std::string o = argStr(0);
      return Value::I(s.size() >= o.size() && s.compare(0, o.size(), o) == 0);
    }
    case M_ENDSWITH: {
      if (!need(1, 1)) return Value();
      std::string o = argStr(0);
      return Value::I(s.size() >= o.size() && s.compare(s.size() - o.size(), o.size(), o) == 0);
    }
    case M_COMPARETO: if (!need(1, 1)) return Value(); return Value::I(s.compare(argStr(0)));
    case M_CONCAT: {
      if (!need(1, 1)) return Value();
      mutStr(obj) += argStr(0);
      return Value::I(1);
    }
    case M_TOUPPER: case M_TOLOWER: {
      if (!need(0, 0)) return Value();
      std::string& m = mutStr(obj);
      for (size_t k = 0; k < m.size(); k++)
        m[k] = (char)(id == M_TOUPPER ? toupper((unsigned char)m[k]) : tolower((unsigned char)m[k]));
      return Value();
    }
    case M_TRIM: {
      if (!need(0, 0)) return Value();
      std::string& m = mutStr(obj);
      size_t a = 0, b = m.size();
      while (a < b && isspace((unsigned char)m[a])) a++;
      while (b > a && isspace((unsigned char)m[b - 1])) b--;
      m = m.substr(a, b - a);
      return Value();
    }
    case M_REPLACE: {
      if (!need(2, 2)) return Value();
      std::string from = av[0].t == V_INT ? std::string(1, (char)av[0].i) : argStr(0);
      std::string to = av[1].t == V_INT ? std::string(1, (char)av[1].i) : argStr(1);
      if (from.empty()) return Value();
      std::string& m = mutStr(obj);
      std::string r;
      size_t p = 0;
      for (;;) {
        size_t q = m.find(from, p);
        if (q == std::string::npos) { r.append(m, p, std::string::npos); break; }
        r.append(m, p, q - p);
        r += to;
        p = q + from.size();
      }
      m = r;
      return Value();
    }
    case M_REMOVE: {
      if (!need(1, 2)) return Value();
      int64_t i = argInt(0), cnt = argc > 1 ? argInt(1) : sl;
      if (i < 0 || i >= sl || cnt <= 0) return Value();
      mutStr(obj).erase((size_t)i, (size_t)cnt);
      return Value();
    }
    case M_SETCHARAT: {
      if (!need(2, 2)) return Value();
      int64_t i = argInt(0);
      if (i < 0 || i >= sl) return Value();
      mutStr(obj)[(size_t)i] = (char)argInt(1);
      return Value();
    }
    default:
      fail(line, "metodo no implementado");
      return Value();
  }
}

RunResult Impl::runFunc(int idx) {
  if (idx < 0) return RUN_FINISHED;
  abort_ = false;
  sp = 0; fp = 0; depth = 0;
  callUser(idx, nullptr, funcs[idx].line);
  if (abort_) {
    if (stopReq.load()) { err = "Detenido"; return RUN_STOPPED; }
    return RUN_ERROR;
  }
  return RUN_OK;
}

// ---------------------------------------------------------------------
//  API publica
// ---------------------------------------------------------------------
Interpreter::Interpreter(Hal& h) : hal(h), impl(nullptr), stopReq(false) {
  impl = new Impl(hal, err, stopReq);
}

Interpreter::~Interpreter() {
  delete impl;
}

void Interpreter::unload() {
  impl->clearProgram();
}

bool Interpreter::isLoaded() const { return impl->loaded; }
bool Interpreter::hasLoop() const { return impl->loaded && impl->loopIdx >= 0; }
size_t Interpreter::programBytes() const { return impl->arena.total; }

bool Interpreter::load(const char* src, size_t len) {
  impl->clearProgram();
  err.clear();
  bool ok;
  {
    Parser p(*impl, src, len);
    ok = p.parseProgram();
    if (!ok) err = p.error;
  }
  if (!ok) {
    impl->clearProgram();
    return false;
  }
  impl->loaded = true;
  return true;
}

RunResult Interpreter::start() {
  if (!impl->loaded) { err = "No hay programa cargado"; return RUN_ERROR; }
  stopReq.store(false);
  err.clear();
  Impl& m = *impl;
  m.abort_ = false;
  m.steps = 0;
  m.rng = hal.entropy() | 1u;
  m.globals.clear();
  m.globals.resize(m.globalTys.size());
  for (size_t k = 0; k < m.globalTys.size(); k++) m.globals[k] = m.defaultValue(m.globalTys[k]);
  m.stack.clear();
  m.stack.resize(STACK_SLOTS);
  m.sp = 0; m.fp = 0; m.depth = 0;
  for (int k = 0; k < 32; k++) m.eventFunc[k] = -1;
  m.eventMask = 0;
  m.inEvent = false;
  hal.takeEvents();
  for (size_t k = 0; k < m.globalInits.size(); k++) {
    m.execDecl(m.globalInits[k]);
    if (m.abort_) {
      if (stopReq.load()) { err = "Detenido"; return RUN_STOPPED; }
      return RUN_ERROR;
    }
  }
  RunResult r = m.runFunc(m.setupIdx);
  if (r == RUN_FINISHED) return m.loopIdx >= 0 ? RUN_OK : RUN_FINISHED;
  if (r == RUN_OK && m.loopIdx < 0) return RUN_FINISHED;
  return r;
}

RunResult Interpreter::loopOnce() {
  if (!impl->loaded) { err = "No hay programa cargado"; return RUN_ERROR; }
  if (impl->loopIdx < 0) return RUN_FINISHED;
  return impl->runFunc(impl->loopIdx);
}

}  // namespace minic
