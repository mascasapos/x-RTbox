// Cada pulsacion del boton (pin 4 a GND) enciende o apaga el LED (pin 2)
const int BOTON = 4;
const int LED = 2;

bool encendido = false;
int anterior = HIGH;
int pulsaciones = 0;

void setup() {
  Serial.begin(115200);
  pinMode(BOTON, INPUT_PULLUP);
  pinMode(LED, OUTPUT);
  Serial.println("Pulsa el boton");
}

void loop() {
  int ahora = digitalRead(BOTON);
  if (anterior == HIGH && ahora == LOW) {   // flanco de bajada = pulsacion
    encendido = !encendido;
    pulsaciones++;
    digitalWrite(LED, encendido ? HIGH : LOW);
    Serial.print("Pulsacion ");
    Serial.print(pulsaciones);
    Serial.println(encendido ? ": LED encendido" : ": LED apagado");
    delay(50);                              // antirrebote
  }
  anterior = ahora;
}
