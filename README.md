# X-RTbox · programa tu ESP32 desde el móvil

App Android + firmware para ESP32. Pegas tu código C++ de Arduino en el móvil, lo guardas, y con un toque se ejecuta en el ESP32 por **Bluetooth** o por **cable USB‑C**, sin PC ni recompilar.

<p align="center"><img src="docs/vista_previa.gif" width="300" alt="Pantalla de inicio de X-RTbox"></p>

```
 MÓVIL (app X-RTbox)                         ESP32 (firmware X-RTbox)
 ┌──────────────────┐  Bluetooth LE      ┌──────────────────────────────┐
 │ CREAR PROGRAMA   │  o cable USB-C     │ recibe el código              │
 │ ALMACÉN  (toque) │ ─────────────────► │ lo guarda en su memoria       │
 │ ENLAZAR ESP32    │ ◄───────────────── │ lo ejecuta con el intérprete  │
 │ consola          │   Serial.print     │ MiniC (C++ estilo Arduino)    │
 └──────────────────┘                    └──────────────────────────────┘
```

**Por qué un intérprete:** el ESP32 no puede compilar C++ por sí mismo, así que el firmware lleva **MiniC**, un intérprete del C++ típico de Arduino que ejecuta tu programa al instante. El firmware se instala **una sola vez**, y lo puede hacer la propia app por USB‑C.

---

## Contenido

| Carpeta | Qué es |
|---|---|
| `esp32/XRTboxESP32/` | Firmware del ESP32: `XRTboxESP32.ino`, `MiniC.h`, `MiniC.cpp` |
| `android/` | App Android (Java, sin librerías externas) |
| `.github/workflows/compilar.yml` + `herramientas/` | Compila el firmware y el APK en GitHub y publica el enlace de descarga |
| `ejemplos/` | Programas de ejemplo listos para pegar en la app |

---

## Paso 1 · Conseguir el APK (descarga directa)

GitHub compila la app gratis y te da un **enlace fijo para descargar el APK desde el móvil**. Además mete dentro de la app el firmware para ESP32, ESP32‑S3, ESP32‑C3 y ESP32‑C6, que necesita el botón *Instalar firmware*.

1. Crea una cuenta en github.com y un repositorio nuevo, por ejemplo `x-rtbox`. Si es **público**, el enlace funciona en cualquier navegador. Si es privado, tienes que tener la sesión de GitHub iniciada en el móvil.
2. Pulsa **"uploading an existing file"**, arrastra **todo el contenido** de la carpeta `X-RTbox` (incluida la carpeta `.github`) y pulsa *Commit changes*.
   - Si tu ordenador oculta `.github` y no se sube, créalo a mano: *Add file → Create new file*, escribe como nombre `.github/workflows/compilar.yml` y pega el contenido de ese archivo (ábrelo con el Bloc de notas).
3. En la pestaña **Actions** verás *Compilar X-RTbox* en marcha (unos 10 minutos la primera vez). Si no arranca, entra y pulsa *Run workflow*.
4. Cuando acabe en verde, abre en el móvil:
   **`https://github.com/TU_USUARIO/x-rtbox/releases/latest/download/X-RTbox.apk`**
   (también lo tienes en la pestaña *Releases* del repositorio). Cada vez que cambies algo, el enlace se actualiza solo.
5. Al abrir el APK, Android pedirá permitir **"instalar apps desconocidas"**. Requiere Android 8.0 o superior.

> Alternativa: compilar con **Android Studio** (*File → Open →* carpeta `android`, *Build → Build APK*). Ese APK no lleva el firmware dentro, a menos que antes copies los `.bin` en `android/app/src/main/assets/firmware/`.

---

## Paso 2 · Poner el firmware en el ESP32 (una sola vez)

### Opción A · Desde el móvil, sin PC (recomendada)

1. Conecta el ESP32 al **USB‑C del móvil**. Vale un cable USB‑C a USB‑C, o un adaptador OTG + el cable de tu placa si es micro‑USB. Tiene que ser un cable de datos: los que solo cargan no sirven.
2. Android preguntará si quieres abrir X-RTbox: di que sí.
3. En **ENLAZAR ESP32** pulsa **⚡ INSTALAR FIRMWARE BLUETOOTH**. La app detecta el chip, lo pone en modo programación y graba el firmware (entre 30 segundos y 2 minutos). El logo gira y se ve el porcentaje.
4. Si dice que no entra en modo programación: mantén pulsado **BOOT**, pulsa y suelta **RST/EN**, suelta **BOOT** y vuelve a intentarlo.

Chips compatibles: ESP32, ESP32‑S3, ESP32‑C3 y ESP32‑C6. El **ESP32‑S2 no tiene Bluetooth** y no sirve. Adaptadores USB compatibles: CP210x, CH340/CH341/CH9102/CH343 y el USB nativo de los S3/C3/C6.

### Opción B · Con el Arduino IDE

1. En *Herramientas → Placa → Gestor de placas*, instala **esp32** de Espressif **versión 3.3.x** (hace falta la 3.x; la 4.0, aún en pruebas, cambió la librería Bluetooth). El firmware te avisa si tienes otra.
2. Abre `esp32/XRTboxESP32/XRTboxESP32.ino`, elige tu placa y en *Partition Scheme* pon **Huge APP (3MB No OTA/1MB SPIFFS)**. En S3/C3/C6 activa también *USB CDC On Boot → Enabled* si vas a usar el cable del USB nativo.
3. Pulsa **Subir**. En el Monitor Serie (115200) verás `[X-RTbox] Bluetooth listo: X-RTbox-A1B2`.

---

## Paso 3 · Usar la app

1. **ENLAZAR ESP32**, en *Conectar por*:
   - **BLUETOOTH**: busca y toca tu `X-RTbox-XXXX`. La próxima vez se reconecta sola al abrir la app.
   - **USB‑C**: con el ESP32 enchufado al móvil. Al enchufarlo, Android ya te ofrece abrir la app y enlazar.
2. **CREAR PROGRAMA** → ponle nombre → *PEGAR* tu código (o *EJEMPLOS* para elegir uno de los que trae la app) → *GUARDAR*, o *GUARDAR Y CARGAR* para mandarlo ya.
3. **ALMACÉN** → **toca un programa y se carga al instante**. Mantén pulsado para editar, renombrar, compartir o eliminar. *IMPORTAR* trae archivos `.ino`/`.cpp`.
   - **Consola:** muestra lo que imprime tu programa con `Serial.print`. Con *ENVIAR* le mandas texto, que lee con `Serial.readStringUntil('\n')`, `Serial.read()`, etc.
   - **■ DETENER** para el programa y deja los pines en reposo. **▶ REINICIAR** lo vuelve a ejecutar desde `setup()`.
   - Si hay un error, la app te dice la línea y te lleva al editor con *IR A LA LÍNEA*.

**Osciloscopio:** en el ALMACÉN, junto al logo, está el botón del osciloscopio. Muestra una pantalla por cada pin de salida que esté usando el programa (fondo negro, señal en verde y rejilla de 10 × 8 divisiones), con la frecuencia, el ciclo de trabajo y la tensión medidos. Con la **cruceta**, ◀ ▶ cambian el tiempo por división (de 20 µs a 1 s) y ▲ ▼ la tensión por división (de 0,1 a 5 V). Toca una gráfica para disparar con ese pin (flanco de subida). El propio ESP32 mide sus pines: las señales digitales (PWM, `digitalWrite`, secuencias…) se leen del registro de entrada y las del DAC/generadores salen con su valor real. Cada punto es la media de todas las lecturas de su intervalo, así que un PWM muy rápido se ve como su valor medio (una SPWM se ve como la senoidal) y, al acercar la escala, como onda cuadrada. Por cable va más fluido que por Bluetooth.

El **logo de radiactividad gira** mientras la app trabaja (buscando, conectando, cargando, instalando firmware…) y se queda quieto al terminar.

El ESP32 **recuerda el último programa** y lo ejecuta solo al encenderse. Si un programa da problemas, enciende el ESP32 con **BOOT** pulsado y arrancará sin ejecutarlo.

**Placas con dos conectores (S3/C3):** para enlazar por cable usa el marcado **USB** (el nativo). Para instalar el firmware vale cualquiera de los dos.

---

## Qué C++ entiende el intérprete

**Sí:**
- `setup()` y `loop()`, funciones propias (con recursión, parámetros por referencia `int &x` y valores por defecto), prototipos
- Tipos: `int`, `long`, `unsigned long`, `byte`, `bool`, `char`, `float`, `double`, `uint8_t`…`int64_t`, `size_t`, `String`, `auto`, `const`, `static`
- Arrays de 1 y 2 dimensiones, `{...}`, `sizeof(arr)/sizeof(arr[0])`, `for (int x : arr)`
- `if/else`, `for`, `while`, `do…while`, `switch/case`, `break`, `continue`, `return`, `?:`
- Todos los operadores, conversiones `(int)x` e `int(x)`
- `#define` (también con parámetros), `#if/#ifdef/#else/#endif`, `enum`, `typedef`; los `#include` se ignoran
- Pines: `pinMode`, `digitalWrite`, `digitalRead`, `analogRead`, `analogWrite`, `analogReadResolution`, `dacWrite`, `touchRead`, `tone`, `noTone`, `pulseIn`, `shiftOut`, `shiftIn`
- Tiempo: `delay`, `delayMicroseconds`, `millis`, `micros`
- Matemáticas: `map`, `constrain`, `min`, `max`, `abs`, `sq`, `pow`, `sqrt`, `sin`/`cos`/`tan`…, `floor`, `ceil`, `round`, `random`, `randomSeed`, `bitRead`/`bitSet`/`bitClear`/`bitWrite`, `lowByte`/`highByte`
- `Serial`: `begin`, `print`, `println` (con `HEX`, `BIN`, decimales), `printf`, `write`, `available`, `read`, `peek`, `readString`, `readStringUntil`, `parseInt`, `parseFloat`
- `String`: `+`, `+=`, `==`, `length`, `substring`, `indexOf`, `charAt`, `toInt`, `toFloat`, `trim`, `toUpperCase`, `toLowerCase`, `replace`, `startsWith`, `endsWith`, `equals`, `remove`, `setCharAt`…, `String(valor)`, `String(valor, HEX)`, `F("texto")`

- `#include <Wire.h>`, `<SPI.h>`, `<ESP32Servo.h>`… se aceptan; estas librerías del ESP32 vienen ya dentro del firmware.

### Librerías y funciones del ESP32

| Para qué | Funciones |
|---|---|
| **PWM (LEDC)**, API del core 3.x | `ledcAttach(pin, Hz, bits)`, `ledcAttachChannel`, `ledcWrite(pin, duty)`, `ledcWriteTone`, `ledcWriteNote`, `ledcRead`, `ledcReadFreq`, `ledcChangeFrequency`, `ledcFade`, `ledcDetach` |
| PWM, API antigua 2.x (canales) | `ledcSetup(canal, Hz, bits)`, `ledcAttachPin(pin, canal)`, `ledcWrite(canal, duty)`, `ledcDetachPin` |
| `analogWrite` | `analogWriteFrequency([pin,] Hz)`, `analogWriteResolution([pin,] bits)` |
| **Generadores de señal** | `signalSquare(pin, Hz, %)` cuadrada por hardware (1 Hz – 40 MHz, en cualquier pin de salida) · `signalSine(pin, Hz, amplitudV, nivelMedioV)` senoidal · `signalTriangle(...)` · `signalSawtooth(...)` (hasta 10 kHz) · `signalStop(pin)`. En español: `cuadrada`, `senoidal`, `triangular`, `dienteDeSierra`, `pararSenal` |
| **Medir** | `frequencyRead(pin, ms)` / `medirFrecuencia` (Hz, hasta unos 100 kHz), `pulseCounterStart/Read/Reset/Stop(pin)`, `pulseIn`, `pulseInLong` |
| **Interrupciones** | `attachInterrupt(digitalPinToInterrupt(pin), funcion, RISING/FALLING/CHANGE)`, `detachInterrupt`, `noInterrupts()`/`interrupts()`, `portENTER_CRITICAL(&mux)`/`portEXIT_CRITICAL`, `IRAM_ATTR`, `volatile` |
| **Temporizadores** | 3.x: `timerBegin(Hz)`, `timerAttachInterrupt(t, &f)`, `timerAlarm(t, valor, repetir, 0)` · 2.x: `timerBegin(0, 80, true)`, `timerAlarmWrite`, `timerAlarmEnable/Disable` · y `timerStart/Stop/Restart/End`, `timerRead`, `timerReadMicros/Millis/Seconds`, `timerWrite` |
| **I2C** | `Wire` y `Wire1`: `begin([SDA, SCL[, Hz]])`, `setClock`, `beginTransmission`, `write`, `endTransmission`, `requestFrom`, `available`, `read`, `peek`, `end` |
| **SPI** | `SPI.begin([sck, miso, mosi, ss])`, `beginTransaction(SPISettings(Hz, MSBFIRST, SPI_MODE0))`, `transfer` (byte o array), `transfer16/32`, `write`, `endTransaction`, `setFrequency`, `setDataMode`, `setBitOrder` |
| **Servos** | `Servo s; s.attach(pin[, minUs, maxUs])`, `write(grados)`, `writeMicroseconds`, `read`, `attached`, `detach` (también en arrays: `Servo dedos[5];`) |
| ADC | `analogReadMilliVolts`, `analogSetAttenuation(ADC_11db)`, `analogSetPinAttenuation`, `A0`…`A19` |
| Sistema | `ESP.getChipModel()`, `getFreeHeap`, `getHeapSize`, `getCpuFreqMHz`, `getChipCores`, `getFlashChipSize`, `getEfuseMac`, `getSdkVersion`, `ESP.restart()`, `temperatureRead()`, `setCpuFrequencyMhz`, `esp_random()`, `esp_timer_get_time()`, `rgbLedWrite(pin, r, g, b)` |
| Constantes | `SDA`, `SCL`, `MOSI`, `MISO`, `SCK`, `SS`, `TX`, `RX`, `DAC1`, `DAC2`, `LED_BUILTIN`, `RGB_BUILTIN`, `NOTE_C`…`NOTE_B` |
| Más PWM (Arduino) | `ledcOutputInvert(pin, true)` (salida invertida), `sigmaDeltaAttach(pin, Hz)`, `sigmaDeltaWrite(pin, 0..255)`, `sigmaDeltaDetach` |

### API de ESP‑IDF (código de bajo nivel de Espressif)

También funciona el código escrito con la API de ESP‑IDF, que es la que suelen dar los ejemplos de Espressif y muchas respuestas de IA:

- **`driver/ledc.h`**: `ledc_timer_config(&t)`, `ledc_channel_config(&c)` con sus estructuras `ledc_timer_config_t` y `ledc_channel_config_t` (también `.flags.output_invert` para señales invertidas y `hpoint` para desfasar), `ledc_set_duty`, `ledc_update_duty`, `ledc_set_duty_and_update`, `ledc_set_duty_with_hpoint`, `ledc_get_duty`, `ledc_set_freq`, `ledc_get_freq`, `ledc_stop`, `ledc_timer_pause/resume/rst`, `ledc_bind_channel_timer`, `ledc_set_pin`, `ledc_fade_func_install`, `ledc_set_fade_with_time`, `ledc_fade_start`… y todas las constantes `LEDC_*` (`LEDC_HIGH_SPEED_MODE` en el ESP32 clásico; en S3/C3/C6 se convierte sola en `LEDC_LOW_SPEED_MODE`).
- **`driver/gpio.h`**: `gpio_config(&io)` con `gpio_config_t`, `gpio_set_level`, `gpio_get_level`, `gpio_set_direction`, `gpio_reset_pin`, `gpio_set_pull_mode`, `GPIO_NUM_x`, `GPIO_MODE_*`…
- **Utilidades**: `ESP_ERROR_CHECK(...)` (si falla, la app te dice qué error y en qué línea), `esp_err_to_name`, `ESP_OK`/`ESP_ERR_*`, `printf`, `ESP_LOGI/W/E`, `vTaskDelay(pdMS_TO_TICKS(ms))`, `portTICK_PERIOD_MS`, `xTaskGetTickCount`, `esp_rom_delay_us`, `BIT(n)`, `memset(&x, 0, sizeof(x))`.
- Un programa con `extern "C" void app_main(void)` (sin `setup`/`loop`) también vale: `app_main` hace de `setup()`.
- **No mezcles** en el mismo programa `ledc_channel_config` con `ledcAttach`/`analogWrite`/`signalSquare`: las dos APIs usan los mismos canales del chip sin saberlo una de la otra.

Ejemplos en la app: *PWM_Invertida_IDF* (dos señales de 1 kHz complementarias) y *PWM_Desfase_IDF* (tres fases desfasadas 120°).

### Registros del GPIO y secuencias por hardware

- **Acceso directo a registros**, como en el código de bajo nivel: `GPIO.out_w1ts = mascara;` (encender), `GPIO.out_w1tc = mascara;` (apagar), `GPIO.out`, `GPIO.enable_w1ts`, `GPIO.in`, `GPIO.out1_w1ts.val` (pines 32‑39)… y también `REG_WRITE(GPIO_OUT_W1TS_REG, BIT(19))`, `REG_READ(GPIO_IN_REG)`, `REG_SET_BIT`, `REG_CLR_BIT`. Los pines de la memoria flash quedan protegidos.
- Ojo: un bucle que cambia pines "lo más rápido posible" funciona, pero interpretado va mucho más lento e irregular que compilado. **Para sacar una secuencia con el tiempo exacto usa el reproductor por hardware**:

```cpp
const int pines[] = {19, 21};
int secuencia[] = {1, 0, 2, 0};   // bit 0 = pin 19, bit 1 = pin 21
patternStart(pines, 2, secuencia, 4, 240);   // 240 pasos por segundo = ciclo de 60 Hz
```

  Un temporizador del ESP32 cambia todos los pines **a la vez** (una sola escritura en el registro) en cada paso, de 0,01 a 100 000 pasos por segundo, sin depender de lo que haga tu programa. Hasta 8 pines y 4096 pasos. También `patternSetFrequency(pasosPorSegundo)` para cambiar la velocidad en marcha y `patternStop()`. En español: `reproducirSecuencia`, `velocidadSecuencia`, `pararSecuencia`. Ejemplo en la app: *Secuencia_SPWM*.

### PWM senoidal (SPWM) con tablas, por hardware

El código típico de SPWM usa una interrupción de temporizador muy rápida (por ejemplo 20 000 veces por segundo) que copia la muestra siguiente de una tabla en el duty del PWM. El intérprete no puede atender tantas interrupciones, así que X‑RTbox **reconoce ese tipo de interrupción y la ejecuta el firmware en C**, con tu código tal cual. Al cargarlo verás en la consola `[X-RTbox] alTick() se ejecuta en el hardware`. Reconoce interrupciones que solo tienen:

- `ledc_set_duty_and_update(modo, canal, tabla[idx], 0);`, o `ledc_set_duty(modo, canal, tabla[idx]); ledc_update_duty(modo, canal);`, o `ledcWrite(pin, tabla[idx]);` (hasta 8 canales);
- y el avance del índice: `if (++idx >= N) idx = 0;`, `idx++; if (idx >= N) idx = 0;` o `idx = (idx + 1) % N;`.

Las tablas se copian al hardware al hacer `timerAttachInterrupt` y `timerAlarm`: si las cambias después, vuelve a llamar a `timerAlarm`. Si la interrupción hace otras cosas, se ejecuta interpretada, y si va a más de 2000 por segundo la consola te avisa de que irá con retraso.

También puedes hacerlo directamente, sin interrupción propia:

```cpp
pwmTableAddPin(19, tablaA, 400);   // o pwmTableAdd(LEDC_HIGH_SPEED_MODE, LEDC_CHANNEL_0, tablaA, 400)
pwmTableAddPin(21, tablaB, 400);
pwmTableStart(20000);              // muestras por segundo: 20000 / 400 = 50 Hz de salida
```

`pwmTableSetRate(muestrasPorSegundo)` cambia la frecuencia en marcha y `pwmTableStop()` lo para (en español: `tablaPWM`, `tablaPWMPin`, `iniciarTablasPWM`, `velocidadTablasPWM`, `pararTablasPWM`). Ejemplos en la app: *SPWM_50Hz_IDF* y *SPWM_Tablas*. También se admiten `sinf`, `cosf`, `sqrtf`, `powf`, `fabsf`…

### Estructuras (`struct`)

`struct Punto { int x; int y = 0; float peso; };`, `typedef struct { ... } Nombre;`, arrays dentro de estructuras, estructuras dentro de estructuras, arrays de estructuras, inicialización con `{1, 2}` o con `{ .x = 1, .y = 2 }`, `p.x = 5`, copiar con `=`, pasarlas a funciones (por valor, por referencia `Punto &p` o como puntero `&p`), devolverlas, `sizeof` y `typedef enum`.

### Señales de 1 kHz (y otras)

- **Cuadrada exacta:** `signalSquare(18, 1000);` → 1000 Hz al 50 % en el pin 18. La genera el periférico LEDC, así que es estable aunque tu programa esté haciendo otras cosas. Con ciclo de trabajo: `signalSquare(18, 1000, 25);`. Lo mismo con la API de siempre: `ledcAttach(18, 1000, 10); ledcWrite(18, 512);`.
- **Senoidal / triangular / diente de sierra:** `signalSine(25, 1000);` (por defecto de 0 a 3,3 V) o `signalSine(25, 1000, 1.0, 1.65);` (1 V de pico alrededor de 1,65 V). En el ESP32 clásico los pines **25 y 26** tienen DAC y la señal sale directamente (40 000 muestras/s, 40 puntos por periodo a 1 kHz). En cualquier otro pin, o en ESP32‑S3/C3/C6 (que no tienen DAC), sale por el modulador **sigma‑delta**: pon un filtro RC (1 kΩ en serie y 47 nF a GND) y verás la onda limpia.
- **Comprobarla sin osciloscopio:** une el pin de salida con otro pin y usa `frequencyRead(4, 1000)` (ejemplo *Senal1kHz*).
- **Interrupciones de temporizador a 1 kHz:** funcionan (ejemplo *TimerInterrupcion*), pero ten en cuenta que en X‑RTbox la función de la interrupción la ejecuta el intérprete justo después de la instrucción en curso: no se pierde ninguna (si llegan varias seguidas se ejecuta una vez por cada una), pero puede retrasarse algunos microsegundos. **Para sacar una señal precisa usa `signalSquare`/`ledc`, no un `digitalWrite` dentro de la interrupción.**

**No (de momento):** `WiFi.h`, `class` y funciones dentro de un `struct`, plantillas, punteros a números (`&x` solo con estructuras, arrays y funciones), `sprintf`/`strcpy`, modo esclavo I2C, tareas de FreeRTOS (`xTaskCreate`) y el MCPWM. La app avisa con un error claro y la línea.

**Velocidad y tamaño:** al ser interpretado va bastante más lento que compilado (del orden de cientos de miles de instrucciones por segundo), de sobra para LEDs, botones, sensores, etc. Las señales (PWM, generadores, temporizadores) las saca el hardware del ESP32, así que no dependen de esa velocidad. Admite programas de hasta 32 KB. Los pines de la memoria flash (6‑11 en el ESP32 clásico) están protegidos.

---

## Problemas frecuentes

| Problema | Solución |
|---|---|
| El USB‑C no detecta el ESP32 | Prueba otro cable (muchos solo cargan). Con placa micro‑USB necesitas un adaptador OTG. Algunos móviles piden activar *OTG* en Ajustes. |
| "No entra en modo programación" | Mantén BOOT, pulsa y suelta RST/EN, suelta BOOT y reintenta. |
| "No se detecta el firmware X-RTbox" al enlazar por USB | El ESP32 aún no lo tiene: pulsa *INSTALAR FIRMWARE BLUETOOTH*. |
| La app no encuentra el ESP32 por Bluetooth | ¿Tiene el firmware? Acércalo. En Android 11 o anterior activa la **ubicación** (lo exige Android para buscar Bluetooth). Solo admite un móvil a la vez. |
| "No se pudo conectar (código 133)" | Error típico de Android: reintenta; si persiste, apaga y enciende el Bluetooth. |
| `Sketch too big` en el Arduino IDE | *Partition Scheme → Huge APP (3MB No OTA/1MB SPIFFS)*. |
| El ESP32 se queda raro | Enciéndelo con BOOT pulsado (no ejecuta el programa guardado) y carga otro. |

---

## Detalles técnicos

- **Bluetooth LE**: servicio `a7c40001-ee5b-4fd0-9052-1429fba680b4`, escritura `…0002`, notificación `…0003`. Nombre anunciado `X-RTbox-XXXX`.
- **Protocolo** (primer carácter = tipo). Móvil → ESP32: `B<longitud>|<crc32>|<nombre>`, `D<datos>`, `E`, `R` reiniciar, `S` detener, `I` estado, `P` ping, `M<mtu>`, `W<texto>`. ESP32 → móvil: `L<salida>`, `K<ok>`, `!<error>`, `T<estado>|<programa>|<memoria>`.
- **Por USB** (115200 baudios) las órdenes van en tramas `0x1E <tipo> <base64> \n` y el ESP32 confirma cada una con `0x1E A<tipo> \n`. Lo que va fuera de tramas es la salida normal del programa, así que también se puede usar un monitor serie normal.
- **Instalador de firmware**: implementa el protocolo del cargador ROM de Espressif (como `esptool`): reinicio por DTR/RTS, sincronización, detección del chip, escritura comprimida a 460800 baudios. Si el adaptador no lo aguanta, reintenta solo a 115200.
- Los programas se comprueban con CRC32 y se guardan en LittleFS (`/xrtbox_programa.cpp`).
