package com.xrtbox.app;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.util.Arrays;
import java.util.zip.DataFormatException;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

/**
 * Graba el firmware en el ESP32 por USB usando el cargador de su ROM
 * (el mismo protocolo que esptool: paquetes SLIP, escritura comprimida).
 */
public class EspFlasher {

    public interface Progress {
        void onProgress(String message, int percent);
    }

    public interface ImageSource {
        /** Imagen completa (grabada desde la direccion 0x0) o null si no existe. */
        byte[] load(String chipAsset) throws IOException;
    }

    public static class FlashException extends IOException {
        public FlashException(String m) {
            super(m);
        }
    }

    private static class Timeout extends IOException {
        Timeout(String m) {
            super(m);
        }
    }

    // ordenes del cargador ROM
    private static final int CMD_SYNC = 0x08;
    private static final int CMD_READ_REG = 0x0A;
    private static final int CMD_SPI_SET_PARAMS = 0x0B;
    private static final int CMD_SPI_ATTACH = 0x0D;
    private static final int CMD_CHANGE_BAUD = 0x0F;
    private static final int CMD_DEFL_BEGIN = 0x10;
    private static final int CMD_DEFL_DATA = 0x11;
    private static final int CMD_SECURITY_INFO = 0x14;

    private static final int BLOCK = 0x400;
    private static final int MAGIC_REG = 0x40001000;
    public static final int FAST_BAUD = 460800;

    /** Chips conocidos. asset = nombre del firmware, null si no se puede instalar. */
    public static final class Chip {
        public final String name;
        public final String asset;
        public final boolean encryptedFlag;   // la ROM espera un campo mas en FLASH_BEGIN
        final int[] magics;
        final int chipId;

        Chip(String name, String asset, boolean enc, int chipId, int... magics) {
            this.name = name;
            this.asset = asset;
            this.encryptedFlag = enc;
            this.chipId = chipId;
            this.magics = magics;
        }
    }

    public static final Chip[] CHIPS = {
        new Chip("ESP32", "esp32", false, 0, 0x00F01D83),
        new Chip("ESP32-S2", null, true, 2, 0x000007C6),
        new Chip("ESP32-S3", "esp32s3", true, 9, 0x00000009),
        new Chip("ESP32-C3", "esp32c3", true, 5, 0x6921506F, 0x1B31506F, 0x4881606F, 0x4361606F),
        new Chip("ESP32-C6", "esp32c6", true, 13, 0x2CE0806F),
        new Chip("ESP32-C2", null, true, 12, 0x6F51306F, 0x7C41A06F),
        new Chip("ESP32-H2", null, true, 16, 0xD7B73E80),
    };

    private final SerialIo io;
    private final Progress progress;
    private final byte[] rbuf = new byte[4096];
    private int rlen = 0;
    private int rpos = 0;
    private int lastPercent = -1;

    public EspFlasher(SerialIo io, Progress progress) {
        this.io = io;
        this.progress = progress;
    }

    private void report(String msg, int pct) {
        lastPercent = pct;
        if (progress != null) progress.onProgress(msg, pct);
    }

    // ------------------------------------------------------------------
    //  Proceso completo
    // ------------------------------------------------------------------
    /** Graba el firmware. Devuelve el nombre del chip. */
    public String run(ImageSource source) throws IOException {
        try {
            return runOnce(source, true);
        } catch (FlashException e) {
            throw e;
        } catch (IOException e) {
            // segundo intento sin subir la velocidad (algunos adaptadores no la aguantan)
            if (io.isNativeUsb()) throw e;
            io.setBaud(115200);
            return runOnce(source, false);
        }
    }

    private String runOnce(ImageSource source, boolean fast) throws IOException {
        report("Poniendo el ESP32 en modo programación...", 0);
        connect();
        Chip chip = detectChip();
        report("Detectado " + chip.name, 2);
        if (chip.asset == null) {
            if (chip.name.equals("ESP32-S2")) throw new FlashException("El ESP32-S2 no tiene Bluetooth: X-RTbox no puede funcionar en él.");
            throw new FlashException("El " + chip.name + " todavía no está soportado por el instalador.");
        }
        byte[] image = source.load(chip.asset);
        if (image == null || image.length == 0) {
            throw new FlashException("Esta versión de la app no incluye el firmware para " + chip.name
                    + ". Usa el APK compilado por GitHub (incluye los firmwares).");
        }
        image = trimTrailingFF(image);
        if (fast && !io.isNativeUsb()) changeBaud(FAST_BAUD);
        check("preparar la memoria", CMD_SPI_ATTACH, new byte[8], 0, 3000);
        ByteArrayOutputStream p = new ByteArrayOutputStream();
        le32(p, 0);
        le32(p, 4 * 1024 * 1024);
        le32(p, 64 * 1024);
        le32(p, 4 * 1024);
        le32(p, 256);
        le32(p, 0xFFFF);
        check("configurar la memoria", CMD_SPI_SET_PARAMS, p.toByteArray(), 0, 3000);
        writeCompressed(chip, image);
        report("Reiniciando el ESP32...", 99);
        hardReset();
        report("Firmware instalado", 100);
        return chip.name;
    }

    // ------------------------------------------------------------------
    //  Reinicio y sincronizacion
    // ------------------------------------------------------------------
    private static void sleep(long ms) {
        try {
            Thread.sleep(ms);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        }
    }

    private void resetIntoBootloader(int attempt) throws IOException {
        if (io.isNativeUsb()) {
            // secuencia del USB-Serial/JTAG (igual que esptool)
            io.setLines(false, false);
            sleep(100);
            io.setLines(true, false);
            sleep(100);
            io.setLines(true, true);
            io.setLines(false, true);
            sleep(100);
            io.setLines(false, false);
        } else {
            // circuito clasico de auto-reset: RTS -> EN, DTR -> IO0
            io.setLines(false, true);    // EN a 0: reinicio
            sleep(100);
            io.setLines(true, false);    // IO0 a 0 y EN a 1: arranca en modo programacion
            sleep(attempt % 2 == 0 ? 50 : 550);
            io.setLines(false, false);
        }
    }

    private void hardReset() throws IOException {
        io.setLines(false, true);
        sleep(150);
        io.setLines(false, false);
    }

    private void drain(long ms) throws IOException {
        long end = System.currentTimeMillis() + ms;
        rpos = rlen = 0;
        byte[] tmp = new byte[1024];
        while (System.currentTimeMillis() < end) {
            io.read(tmp, (int) Math.max(1, end - System.currentTimeMillis()));
        }
    }

    private boolean trySync() throws IOException {
        byte[] d = new byte[36];
        d[0] = 0x07;
        d[1] = 0x07;
        d[2] = 0x12;
        d[3] = 0x20;
        Arrays.fill(d, 4, 36, (byte) 0x55);
        try {
            command(CMD_SYNC, d, 0, 150);
        } catch (Timeout t) {
            return false;
        }
        drain(120);   // la ROM contesta varias veces a la sincronizacion
        return true;
    }

    private void connect() throws IOException {
        for (int attempt = 0; attempt < 6; attempt++) {
            resetIntoBootloader(attempt);
            drain(60);
            for (int k = 0; k < 5; k++) {
                if (trySync()) return;
            }
        }
        throw new FlashException("El ESP32 no entra en modo programación. Mantén pulsado el botón BOOT, "
                + "pulsa y suelta RST (o EN), suelta BOOT y vuelve a intentarlo.");
    }

    private Chip detectChip() throws IOException {
        int magic = check("identificar el chip", CMD_READ_REG, le32(MAGIC_REG), 0, 1000).value;
        for (Chip c : CHIPS) {
            for (int m : c.magics) if (m == magic) return c;
        }
        // chips nuevos: pedir el identificador
        try {
            Resp r = check("identificar el chip", CMD_SECURITY_INFO, new byte[0], 0, 1000);
            if (r.data.length >= 16) {
                int id = u32(r.data, 12);
                for (Chip c : CHIPS) if (c.chipId == id && id != 0) return c;
            }
        } catch (IOException e) {
            // sin informacion
        }
        throw new FlashException(String.format("Chip desconocido (0x%08X): no se puede instalar el firmware.", magic));
    }

    private void changeBaud(int baud) throws IOException {
        ByteArrayOutputStream p = new ByteArrayOutputStream();
        le32(p, baud);
        le32(p, 0);
        check("cambiar la velocidad", CMD_CHANGE_BAUD, p.toByteArray(), 0, 1500);
        io.setBaud(baud);
        sleep(60);
        drain(40);
        // comprobar que seguimos entendiendonos
        check("comprobar la velocidad", CMD_READ_REG, le32(MAGIC_REG), 0, 1000);
    }

    // ------------------------------------------------------------------
    //  Escritura comprimida
    // ------------------------------------------------------------------
    /** Quita el relleno vacio (0xFF) del final: no hace falta borrarlo ni escribirlo. */
    static byte[] trimTrailingFF(byte[] img) {
        int end = img.length;
        while (end > 0 && img[end - 1] == (byte) 0xFF) end--;
        end = ((end + 4095) / 4096) * 4096;
        if (end <= 0) end = Math.min(img.length, 4096);
        if (end >= img.length) return img;
        return Arrays.copyOf(img, end);
    }

    static byte[] deflate(byte[] in) {
        Deflater d = new Deflater(Deflater.BEST_COMPRESSION);
        d.setInput(in);
        d.finish();
        ByteArrayOutputStream out = new ByteArrayOutputStream(in.length / 2 + 64);
        byte[] buf = new byte[16384];
        while (!d.finished()) {
            int n = d.deflate(buf);
            out.write(buf, 0, n);
        }
        d.end();
        return out.toByteArray();
    }

    private void writeCompressed(Chip chip, byte[] image) throws IOException {
        report("Comprimiendo el firmware...", 3);
        byte[] comp = deflate(image);
        int blocks = (comp.length + BLOCK - 1) / BLOCK;
        int eraseSize = ((image.length + BLOCK - 1) / BLOCK) * BLOCK;
        ByteArrayOutputStream p = new ByteArrayOutputStream();
        le32(p, eraseSize);
        le32(p, blocks);
        le32(p, BLOCK);
        le32(p, 0);                     // direccion 0x0: imagen completa
        if (chip.encryptedFlag) le32(p, 0);
        report("Borrando la memoria del " + chip.name + "...", 4);
        int eraseTimeout = (int) Math.max(15000, 35L * eraseSize / 1000);   // ~35 s por MB
        check("borrar la memoria", CMD_DEFL_BEGIN, p.toByteArray(), 0, eraseTimeout);
        // Para cada bloque se calcula cuanto ocupa descomprimido (como esptool):
        // un bloque de relleno puede ser mucho mayor y la ROM tarda mas en escribirlo.
        Inflater meter = new Inflater();
        byte[] scratch = new byte[65536];
        for (int seq = 0; seq < blocks; seq++) {
            int off = seq * BLOCK;
            int n = Math.min(BLOCK, comp.length - off);
            long produced = 0;
            meter.setInput(comp, off, n);
            try {
                while (!meter.needsInput() && !meter.finished()) {
                    int k = meter.inflate(scratch);
                    if (k == 0) break;
                    produced += k;
                }
            } catch (DataFormatException e) {
                produced = BLOCK * 64L;
            }
            int blockTimeout = (int) Math.max(15000, 45L * produced / 1000 + 5000);   // ~45 s por MB
            ByteArrayOutputStream d = new ByteArrayOutputStream(n + 16);
            le32(d, n);
            le32(d, seq);
            le32(d, 0);
            le32(d, 0);
            d.write(comp, off, n);
            int chk = 0xEF;
            for (int k = off; k < off + n; k++) chk ^= comp[k] & 0xFF;
            byte[] payload = d.toByteArray();
            for (int attempt = 0; ; attempt++) {
                try {
                    check("escribir el firmware", CMD_DEFL_DATA, payload, chk, blockTimeout);
                    break;
                } catch (Timeout t) {
                    if (attempt >= 2) throw new FlashException("El ESP32 dejó de responder mientras se grababa. Vuelve a intentarlo.");
                }
            }
            int pct = 5 + (int) (93L * (seq + 1) / blocks);
            if (pct != lastPercent) report("Grabando firmware...", pct);
        }
        meter.end();
        // Con la ROM no se manda FLASH_END: haria arrancar el programa antes de tiempo.
    }

    // ------------------------------------------------------------------
    //  Paquetes SLIP
    // ------------------------------------------------------------------
    static final class Resp {
        int value;
        byte[] data;
    }

    private void writeSlip(byte[] pkt) throws IOException {
        ByteArrayOutputStream o = new ByteArrayOutputStream(pkt.length + 16);
        o.write(0xC0);
        for (byte b : pkt) {
            int v = b & 0xFF;
            if (v == 0xC0) {
                o.write(0xDB);
                o.write(0xDC);
            } else if (v == 0xDB) {
                o.write(0xDB);
                o.write(0xDD);
            } else {
                o.write(v);
            }
        }
        o.write(0xC0);
        io.write(o.toByteArray());
    }

    private int readByte(long deadline) throws IOException {
        while (rpos >= rlen) {
            long left = deadline - System.currentTimeMillis();
            if (left <= 0) return -1;
            rlen = io.read(rbuf, (int) Math.min(left, 200));
            rpos = 0;
        }
        return rbuf[rpos++] & 0xFF;
    }

    private byte[] readPacket(long deadline) throws IOException {
        int b;
        do {
            b = readByte(deadline);
            if (b < 0) return null;
        } while (b != 0xC0);
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (;;) {
            b = readByte(deadline);
            if (b < 0) return null;
            if (b == 0xC0) {
                if (out.size() == 0) continue;   // separador repetido
                return out.toByteArray();
            }
            if (b == 0xDB) {
                int c = readByte(deadline);
                if (c < 0) return null;
                out.write(c == 0xDC ? 0xC0 : (c == 0xDD ? 0xDB : c));
            } else {
                out.write(b);
            }
        }
    }

    private Resp command(int op, byte[] data, int chk, int timeoutMs) throws IOException {
        byte[] pkt = new byte[8 + data.length];
        pkt[0] = 0;
        pkt[1] = (byte) op;
        pkt[2] = (byte) data.length;
        pkt[3] = (byte) (data.length >> 8);
        pkt[4] = (byte) chk;
        pkt[5] = (byte) (chk >> 8);
        pkt[6] = (byte) (chk >> 16);
        pkt[7] = (byte) (chk >> 24);
        System.arraycopy(data, 0, pkt, 8, data.length);
        writeSlip(pkt);
        long deadline = System.currentTimeMillis() + timeoutMs;
        for (;;) {
            byte[] p = readPacket(deadline);
            if (p == null) throw new Timeout("Sin respuesta del ESP32");
            if (p.length < 8 || p[0] != 1 || (p[1] & 0xFF) != op) continue;
            int size = (p[2] & 0xFF) | ((p[3] & 0xFF) << 8);
            Resp r = new Resp();
            r.value = u32(p, 4);
            r.data = Arrays.copyOfRange(p, 8, Math.min(p.length, 8 + size));
            return r;
        }
    }

    private Resp check(String what, int op, byte[] data, int chk, int timeoutMs) throws IOException {
        Resp r = command(op, data, chk, timeoutMs);
        if (r.data.length < 2) throw new FlashException("Respuesta no válida al " + what);
        int statusLen = r.data.length >= 4 ? 4 : 2;
        int st = r.data.length - statusLen;
        if (r.data[st] != 0) {
            throw new FlashException("Error al " + what + " (código " + (r.data[st + 1] & 0xFF) + ")");
        }
        return r;
    }

    // ------------------------------------------------------------------
    //  Utilidades
    // ------------------------------------------------------------------
    private static byte[] le32(int v) {
        return new byte[]{(byte) v, (byte) (v >> 8), (byte) (v >> 16), (byte) (v >> 24)};
    }

    private static void le32(ByteArrayOutputStream o, int v) {
        o.write(v & 0xFF);
        o.write((v >> 8) & 0xFF);
        o.write((v >> 16) & 0xFF);
        o.write((v >> 24) & 0xFF);
    }

    private static int u32(byte[] b, int off) {
        return (b[off] & 0xFF) | ((b[off + 1] & 0xFF) << 8) | ((b[off + 2] & 0xFF) << 16) | ((b[off + 3] & 0xFF) << 24);
    }
}
