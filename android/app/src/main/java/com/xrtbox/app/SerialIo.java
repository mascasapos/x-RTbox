package com.xrtbox.app;

import java.io.IOException;

/** Puerto serie minimo (lo usa el instalador de firmware; facil de simular en pruebas). */
public interface SerialIo {
    void write(byte[] data) throws IOException;

    /** Lee lo que haya; devuelve 0 si no llega nada en el tiempo indicado. */
    int read(byte[] buf, int timeoutMs) throws IOException;

    void setBaud(int baud) throws IOException;

    /** Lineas de control: en las placas ESP32 sirven para reiniciar y entrar en modo programacion. */
    void setLines(boolean dtr, boolean rts) throws IOException;

    /** true si es el USB nativo del ESP32 (S3, C3, C6...) en vez de un chip USB-serie. */
    boolean isNativeUsb();
}
