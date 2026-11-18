// Escribe ordenes en la consola de la app (ALMACEN -> ENVIAR):
//   on   off   blink 5   estado
const int LED = 2;
bool estado = false;

void setup() {
  Serial.begin(115200);
  pinMode(LED, OUTPUT);
  Serial.println("Ordenes: on, off, blink N, estado");
}

void parpadea(int veces) {
  for (int i = 0; i < veces; i++) {
    digitalWrite(LED, HIGH);
    delay(200);
    digitalWrite(LED, LOW);
    delay(200);
  }
  digitalWrite(LED, estado ? HIGH : LOW);
}

void loop() {
  if (Serial.available() > 0) {
    String orden = Serial.readStringUntil('\n');
    orden.trim();
    orden.toLowerCase();
    if (orden == "on") {
      estado = true;
      digitalWrite(LED, HIGH);
      Serial.println("LED encendido");
    } else if (orden == "off") {
      estado = false;
      digitalWrite(LED, LOW);
      Serial.println("LED apagado");
    } else if (orden.startsWith("blink")) {
      int n = orden.substring(5).toInt();
      if (n <= 0) n = 3;
      Serial.println("Parpadeando " + String(n) + " veces");
      parpadea(n);
    } else if (orden == "estado") {
      Serial.println(estado ? "El LED esta encendido" : "El LED esta apagado");
    } else if (orden.length() > 0) {
      Serial.println("No entiendo: " + orden);
    }
  }
}
