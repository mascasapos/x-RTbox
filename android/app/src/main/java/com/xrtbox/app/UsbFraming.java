package com.xrtbox.app;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Base64;

/**
 * Protocolo por cable USB. Las ordenes van en tramas de texto para poder
 * mezclarse con la salida normal del programa (Serial.print):
 *
 *   movil -> ESP32:  0x1E <tipo> <datos en base64> '\n'
 *   ESP32 -> movil:  0x1E <tipo> <texto> '\n'      (tipo 'A' = acuse de recibo)
 *
 * Todo lo que llega fuera de una trama es salida del programa.
 */
public final class UsbFraming {

    public static final byte START = 0x1E;
    private static final int MAX_FRAME = 2048;

    private UsbFraming() {
    }

    public static byte[] encode(char type, byte[] payload) {
        byte[] b64 = Base64.getEncoder().encode(payload);
        byte[] out = new byte[b64.length + 3];
        out[0] = START;
        out[1] = (byte) type;
        System.arraycopy(b64, 0, out, 2, b64.length);
        out[out.length - 1] = '\n';
        return out;
    }

    public interface Sink {
        /** Bytes de salida normal del programa. */
        void onText(byte[] data);

        /** Trama de control recibida (tipo y texto). */
        void onFrame(char type, String text);
    }

    /** Separa el flujo de bytes en salida normal y tramas. */
    public static final class Parser {
        private final Sink sink;
        private final ByteArrayOutputStream frame = new ByteArrayOutputStream();
        private final ByteArrayOutputStream text = new ByteArrayOutputStream();
        private boolean inFrame;

        public Parser(Sink sink) {
            this.sink = sink;
        }

        public void feed(byte[] data) {
            for (byte b : data) {
                if (inFrame) {
                    if (b == '\n') {
                        inFrame = false;
                        byte[] f = frame.toByteArray();
                        frame.reset();
                        if (f.length > 0) {
                            sink.onFrame((char) (f[0] & 0xFF), new String(f, 1, f.length - 1, StandardCharsets.UTF_8));
                        }
                    } else if (b == START) {
                        frame.reset();
                    } else if (b != '\r') {
                        if (frame.size() < MAX_FRAME) frame.write(b);
                        else {
                            inFrame = false;
                            frame.reset();
                        }
                    }
                } else if (b == START) {
                    flushText();
                    inFrame = true;
                    frame.reset();
                } else {
                    text.write(b);
                }
            }
            flushText();
        }

        private void flushText() {
            if (text.size() == 0) return;
            byte[] t = text.toByteArray();
            text.reset();
            sink.onText(t);
        }
    }
}
