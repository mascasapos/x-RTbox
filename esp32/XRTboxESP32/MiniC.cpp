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
  TY_I64, TY_U64, TY_FLOAT, TY_DOUBLE, TY_STRING, TY_ANY, TY__COUNT
};
static const uint8_t kTyBits[TY__COUNT] = {0, 1, 8, 8, 8, 16, 16, 32, 32, 64, 64, 0, 0, 0, 0};
static const bool kTyUns[TY__COUNT] = {false, true, false, true, false, false, true, false, true,
                                       false, true, false, false, false, false};
static const uint8_t kTySize[TY__COUNT] = {0, 1, 1, 1, 1, 2, 2, 4, 4, 8, 8, 4, 8, 12, 4};
static inline bool tyIsInt(uint8_t t) { return t >= TY_BOOL && t <= TY_U64; }

// ---------------------------------------------------------------------
//  Valores en tiempo de ejecucion
// ---------------------------------------------------------------------
enum VT : uint8_t { V_VOID, V_INT, V_FLT, V_STR, V_ARR, V_REF };

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
  bool isNum() const { return t == V_INT || t == V_FLT; }
};

struct StrObj {
  int32_t rc;
  std::string s;
};

struct ArrObj {
  int32_t rc;
  uint8_t elemTy;
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
  N_RETURN, N_SWITCH, N_CASE, N_DEFAULT, N_EMPTY, N_DIM
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

static int findBuiltin(const std::string& name) {
  for (int k = 0; k < B__COUNT; k++)
    if (name == kBuiltins[k].name) return k;
  return -1;
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
    if (t.text == "PROGMEM" || t.text == "IRAM_ATTR" || t.text == "ICACHE_RAM_ATTR" || t.text == "DRAM_ATTR")
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

  Impl(Hal& h, std::string& e, std::atomic<bool>& s)
      : hal(h), err(e), stopReq(s), setupIdx(-1), loopIdx(-1), loaded(false), sp(0), fp(0), depth(0),
        abort_(false), steps(0), rng(0x2545F491u) {}

  void clearProgram() {
    globals.clear();
    stack.clear();
    retVal = Value();
    lrefHold = Value();
    funcs.clear();
    strConsts.clear();
    globalInits.clear();
    globalTys.clear();
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
      case V_STR: case V_ARR: return true;
      default: return false;
    }
  }
  const char* kindName(const Value& v) {
    switch (v.t) {
      case V_STR: return "un String";
      case V_ARR: return "un array";
      case V_VOID: return "un valor vacio (void)";
      default: return "un numero";
    }
  }
  Value defaultValue(uint8_t ty) {
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
      case V_ARR: return "[array]";
      default: return "";
    }
  }

  Value coerce(Value v, uint8_t ty, int line) {
    if (v.t == V_REF) { Value d = *v.r; v = d; }
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
  bool parseEnum(bool global);
  bool parseTypedef();
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
  bool isLvalue(Node* n) { return n && (n->kind == N_LVAR || n->kind == N_GVAR || n->kind == N_INDEX); }
  bool resolveCalls();
};

bool Parser::isTypeWord(const std::string& s) {
  static const char* const words[] = {
    "void", "bool", "boolean", "char", "byte", "short", "int", "long", "unsigned", "signed", "float",
    "double", "String", "size_t", "uint8_t", "int8_t", "uint16_t", "int16_t", "uint32_t", "int32_t",
    "uint64_t", "int64_t", "word", "auto", "const", "constexpr", "static", "volatile", "inline", "extern",
    "register", nullptr};
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

bool Parser::parseProgram() {
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
    if (t.text == "struct" || t.text == "class" || t.text == "union" || t.text == "template" || t.text == "namespace") {
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

bool Parser::parseEnum(bool global) {
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
  if (peek().type == TK_IDENT) { failAt(line, "declarar variables en la misma linea del enum no esta soportado"); return false; }
  return expectOp(';');
}

bool Parser::parseTypedef() {
  int line = takeLine();
  if (isIdent("struct") || isIdent("union") || isIdent("enum")) {
    failAt(line, "typedef de struct/enum no esta soportado");
    return false;
  }
  bool a, b, c;
  int ty = parseType(a, b, false, c);
  std::string nm;
  if (!expectIdent(nm)) return false;
  typeAliases[nm] = (uint8_t)ty;
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
    Node* e = isOp('{') ? parseInitList() : parseAssign();
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
    if (s == "struct" || s == "class") { failAt(line, "'%s' no esta soportado por el interprete", s.c_str()); return nullptr; }
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
      case '&': failAt(line, "los punteros (&x) no estan soportados; usa parametros por referencia"); return nullptr;
      default: break;
    }
    if (kind >= 0) {
      skip();
      if (++nest > MAX_PARSE_NEST) { failAt(line, "expresion demasiado anidada"); return nullptr; }
      Node* a = parseUnary();
      nest--;
      if (failed) return nullptr;
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
    {"NULL", 0}, {"nullptr", 0}, {"RISING", 1}, {"FALLING", 2}, {"CHANGE", 3}, {"INT_MAX", 2147483647LL},
    {"INT_MIN", -2147483647LL - 1}, {"UINT32_MAX", 4294967295LL}, {"LONG_MAX", 2147483647LL}, {nullptr, 0}};
  for (int k = 0; ki[k].n; k++) {
    if (s == ki[k].n) {
      Node* n = mk(N_NUM, line);
      if (n) n->ival = ki[k].v;
      return n;
    }
  }
  if (s == "LED_BUILTIN") {
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
  failAt(line, "'%s' no esta declarado", s.c_str());
  return nullptr;
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
  // Serial.xxx(...)
  if (s == "Serial" || s == "Serial0" || s == "USBSerial") {
    if (isOp('.')) {
      skip();
      std::string m;
      if (!expectIdent(m)) return nullptr;
      std::string full = "Serial." + m;
      if (findBuiltin(full) < 0) { failAt(line, "Serial.%s() no esta soportado", m.c_str()); return nullptr; }
      if (!expectOp('(')) return nullptr;
      int argc;
      Node* args = parseArgs(argc);
      if (failed) return nullptr;
      Node* n = mk(N_PCALL, line);
      if (!n) return nullptr;
      n->b = args; n->ival = argc;
      PendingCall pc; pc.n = n; pc.name = full;
      pendingCalls.push_back(pc);
      return n;
    }
    Node* n = mk(N_NUM, line);  // while(!Serial)
    if (n) n->ival = 1;
    return n;
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
    if (s == "attachInterrupt" || s == "detachInterrupt") {
      failAt(line, "las interrupciones (%s) no estan soportadas; lee el pin dentro de loop()", s.c_str());
      return nullptr;
    }
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
      const BuiltinDef& bd = kBuiltins[bi];
      if (argc < bd.minA || argc > bd.maxA) {
        if (bd.minA == bd.maxA) failAt(n->line, "%s() necesita %d argumento(s) y se le pasan %d", name.c_str(), bd.minA, argc);
        else failAt(n->line, "%s() necesita entre %d y %d argumentos y se le pasan %d", name.c_str(), bd.minA, bd.maxA, argc);
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
    if (name == "attachInterrupt" || name == "detachInterrupt")
      failAt(n->line, "las interrupciones (%s) no estan soportadas; lee el pin dentro de loop()", name.c_str());
    else if (name == "sprintf" || name == "snprintf" || name == "strcpy" || name == "strcat" || name == "malloc")
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
  if (dst->t == V_ARR) { fail(n->line, "no se puede asignar un array completo; asigna elemento a elemento"); return Value(); }
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
  if (a->v.empty()) return 0;
  if (a->v[0].t == V_ARR) return (int64_t)a->v.size() * arrBytes(a->v[0].a);
  return (int64_t)a->v.size() * kTySize[a->elemTy];
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
      if (!n->a) return Value::I(kTySize[n->op]);
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
      return Value::I(kTySize[ty]);
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
        if (a->kind != N_LVAR && a->kind != N_GVAR && a->kind != N_INDEX) {
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

bool Impl::sleepMs(uint32_t ms) {
  uint32_t start = hal.millis();
  for (;;) {
    if (checkStop()) return false;
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
