package com.xrtbox.app;

import android.hardware.usb.UsbConstants;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbDeviceConnection;
import android.hardware.usb.UsbEndpoint;
import android.hardware.usb.UsbInterface;
import android.hardware.usb.UsbManager;

import java.io.IOException;
import java.util.Arrays;

/**
 * Puerto serie por USB (USB host / OTG) sin librerias externas.
 * Soporta los chips habituales de las placas ESP32:
 *  - CDC-ACM: USB nativo de ESP32-S3/C3/C6 y chips CH9102/CH343
 *  - Silicon Labs CP210x (CP2102, CP2104...)
 *  - WCH CH340 / CH341
 */
public class UsbSerial implements SerialIo {

    public static final int KIND_CDC = 0;
    public static final int KIND_CP210X = 1;
    public static final int KIND_CH34X = 2;

    private static final int VID_SILABS = 0x10C4;
    private static final int VID_WCH = 0x1A86;
    private static final int VID_ESPRESSIF = 0x303A;
    private static final int TIMEOUT = 2000;

    public interface DataListener {
        void onData(byte[] data);
    }

    private final UsbDevice device;
    private final int kind;
    private UsbDeviceConnection conn;
    private UsbInterface ctrlIface;
    private UsbInterface dataIface;
    private UsbEndpoint epIn;
    private UsbEndpoint epOut;
    private int ctrlIndex;
    private int baud = 115200;
    private boolean dtr, rts;
    private volatile boolean reading;
    private Thread reader;

    public UsbSerial(UsbDevice d) {
        device = d;
        kind = kindOf(d);
    }

    public UsbDevice getDevice() {
        return device;
    }

    // ------------------------------------------------------------------
    //  Deteccion
    // ------------------------------------------------------------------
    private static boolean hasCdcInterfaces(UsbDevice d) {
        for (int i = 0; i < d.getInterfaceCount(); i++) {
            int c = d.getInterface(i).getInterfaceClass();
            if (c == UsbConstants.USB_CLASS_COMM || c == UsbConstants.USB_CLASS_CDC_DATA) return true;
        }
        return false;
    }

    /** Tipo de chip, o -1 si no es un puerto serie conocido. */
    public static int kindOf(UsbDevice d) {
        int vid = d.getVendorId();
        if (vid == VID_SILABS) return KIND_CP210X;
        if (vid == VID_WCH) return hasCdcInterfaces(d) ? KIND_CDC : KIND_CH34X;
        if (vid == VID_ESPRESSIF) return KIND_CDC;
        if (hasCdcInterfaces(d)) return KIND_CDC;
        return -1;
    }

    public static boolean isSupported(UsbDevice d) {
        return kindOf(d) >= 0;
    }

    public static String describe(UsbDevice d) {
        int vid = d.getVendorId();
        int pid = d.getProductId();
        if (vid == VID_ESPRESSIF) return "ESP32 (USB nativo)";
        if (vid == VID_SILABS) return "CP210x";
        if (vid == VID_WCH) {
            if (pid == 0x7523) return "CH340";
            if (pid == 0x5523) return "CH341";
            if (pid == 0x55D4) return "CH9102";
            if (pid == 0x55D3) return "CH343";
            return "CH34x";
        }
        return "USB serie";
    }

    @Override
    public boolean isNativeUsb() {
        return device.getVendorId() == VID_ESPRESSIF;
    }

    // ------------------------------------------------------------------
    //  Abrir / cerrar
    // ------------------------------------------------------------------
    public void open(UsbManager manager) throws IOException {
        if (kind < 0) throw new IOException("Este dispositivo USB no es un puerto serie compatible");
        conn = manager.openDevice(device);
        if (conn == null) throw new IOException("No se pudo abrir el USB (¿falta el permiso?)");
        try {
            switch (kind) {
                case KIND_CP210X:
                    openSingleInterface();
                    vendorOut(0x00, 0x0001, null);         // IFC_ENABLE
                    vendorOut(0x03, 0x0800, null);         // 8 bits, sin paridad, 1 stop
                    setBaud(baud);
                    setLines(false, false);
                    break;
                case KIND_CH34X:
                    openSingleInterface();
                    initCh34x();
                    break;
                default:
                    openCdc();
                    setBaud(baud);
                    setLines(false, false);
                    break;
            }
        } catch (IOException e) {
            close();
            throw e;
        }
    }

    private void findBulk(UsbInterface iface) {
        for (int k = 0; k < iface.getEndpointCount(); k++) {
            UsbEndpoint ep = iface.getEndpoint(k);
            if (ep.getType() != UsbConstants.USB_ENDPOINT_XFER_BULK) continue;
            if (ep.getDirection() == UsbConstants.USB_DIR_IN) epIn = ep;
            else epOut = ep;
        }
    }

    private void openSingleInterface() throws IOException {
        if (device.getInterfaceCount() == 0) throw new IOException("Dispositivo USB sin interfaces");
        dataIface = device.getInterface(0);
        if (!conn.claimInterface(dataIface, true)) throw new IOException("No se pudo usar el puerto USB");
        ctrlIndex = dataIface.getId();
        findBulk(dataIface);
        if (epIn == null || epOut == null) throw new IOException("El puerto USB no tiene canales de datos");
    }

    private void openCdc() throws IOException {
        for (int i = 0; i < device.getInterfaceCount(); i++) {
            UsbInterface it = device.getInterface(i);
            if (it.getInterfaceClass() == UsbConstants.USB_CLASS_COMM && ctrlIface == null) ctrlIface = it;
            else if (it.getInterfaceClass() == UsbConstants.USB_CLASS_CDC_DATA && dataIface == null) dataIface = it;
        }
        if (dataIface == null) {
            // algunos dispositivos lo ponen todo en una sola interfaz
            for (int i = 0; i < device.getInterfaceCount() && dataIface == null; i++) {
                UsbInterface it = device.getInterface(i);
                epIn = null;
                epOut = null;
                findBulk(it);
                if (epIn != null && epOut != null) dataIface = it;
            }
        }
        if (dataIface == null) throw new IOException("No se encontro el canal de datos del USB");
        if (ctrlIface != null && !conn.claimInterface(ctrlIface, true)) throw new IOException("No se pudo usar el puerto USB");
        if (!conn.claimInterface(dataIface, true)) throw new IOException("No se pudo usar el puerto USB");
        ctrlIndex = ctrlIface != null ? ctrlIface.getId() : dataIface.getId();
        epIn = null;
        epOut = null;
        findBulk(dataIface);
        if (epIn == null || epOut == null) throw new IOException("El puerto USB no tiene canales de datos");
    }

    public void close() {
        stopReading();
        if (conn != null) {
            try {
                if (dataIface != null) conn.releaseInterface(dataIface);
                if (ctrlIface != null) conn.releaseInterface(ctrlIface);
            } catch (Exception e) {
                // ignorar
            }
            conn.close();
            conn = null;
        }
    }

    // ------------------------------------------------------------------
    //  Control
    // ------------------------------------------------------------------
    private void vendorOut(int request, int value, byte[] data) throws IOException {
        int len = data == null ? 0 : data.length;
        int r = conn.controlTransfer(0x41, request, value, ctrlIndex, data, len, TIMEOUT);
        if (r < 0) throw new IOException("Error de control USB (" + request + ")");
    }

    private void chOut(int request, int value, int index) throws IOException {
        int r = conn.controlTransfer(0x41, request, value & 0xFFFF, index & 0xFFFF, null, 0, TIMEOUT);
        if (r < 0) throw new IOException("Error de control USB CH34x (" + request + ")");
    }

    private void chIn(int request, int value, int index) {
        byte[] b = new byte[2];
        conn.controlTransfer(0xC0, request, value, index, b, 2, TIMEOUT);
    }

    private void initCh34x() throws IOException {
        chIn(0x5F, 0, 0);
        chOut(0xA1, 0, 0);
        chBaud(baud);
        chIn(0x95, 0x2518, 0);
        chOut(0x9A, 0x2518, 0x00C3);   // activar RX/TX, 8 bits
        chIn(0x95, 0x0706, 0);
        chOut(0xA1, 0x501F, 0xD90A);
        chBaud(baud);
        setLines(false, false);
    }

    private void chBaud(int b) throws IOException {
        long factor;
        long divisor;
        if (b == 921600) {
            divisor = 7;
            factor = 0xF300;
        } else {
            factor = 1532620800L / b;
            divisor = 3;
            while (factor > 0xFFF0 && divisor > 0) {
                factor >>= 3;
                divisor--;
            }
            if (factor > 0xFFF0) throw new IOException("Velocidad no soportada: " + b);
            factor = 0x10000 - factor;
        }
        divisor |= 0x0080;
        chOut(0x9A, 0x1312, (int) ((factor & 0xFF00) | divisor));
        chOut(0x9A, 0x0F2C, (int) (factor & 0xFF));
    }

    @Override
    public void setBaud(int b) throws IOException {
        baud = b;
        if (conn == null) return;
        switch (kind) {
            case KIND_CP210X: {
                byte[] d = {(byte) b, (byte) (b >> 8), (byte) (b >> 16), (byte) (b >> 24)};
                vendorOut(0x1E, 0, d);
                break;
            }
            case KIND_CH34X:
                chBaud(b);
                break;
            default: {
                byte[] d = {(byte) b, (byte) (b >> 8), (byte) (b >> 16), (byte) (b >> 24), 0, 0, 8};
                int r = conn.controlTransfer(0x21, 0x20, 0, ctrlIndex, d, d.length, TIMEOUT);
                if (r < 0 && !isNativeUsb()) throw new IOException("No se pudo configurar la velocidad");
                break;
            }
        }
    }

    @Override
    public void setLines(boolean newDtr, boolean newRts) throws IOException {
        dtr = newDtr;
        rts = newRts;
        if (conn == null) return;
        switch (kind) {
            case KIND_CP210X: {
                int v = (dtr ? 0x0001 : 0) | (rts ? 0x0002 : 0) | 0x0100 | 0x0200;
                vendorOut(0x07, v, null);
                break;
            }
            case KIND_CH34X:
                chOut(0xA4, ~((dtr ? 0x20 : 0) | (rts ? 0x40 : 0)), 0);
                break;
            default: {
                int v = (dtr ? 1 : 0) | (rts ? 2 : 0);
                conn.controlTransfer(0x21, 0x22, v, ctrlIndex, null, 0, TIMEOUT);
                break;
            }
        }
    }

    // ------------------------------------------------------------------
    //  Datos
    // ------------------------------------------------------------------
    @Override
    public void write(byte[] data) throws IOException {
        UsbDeviceConnection c = conn;
        if (c == null) throw new IOException("USB cerrado");
        int off = 0;
        while (off < data.length) {
            int n = Math.min(data.length - off, 16384);
            int w = c.bulkTransfer(epOut, data, off, n, TIMEOUT);
            if (w <= 0) throw new IOException("Error al enviar por USB");
            off += w;
        }
    }

    @Override
    public int read(byte[] buf, int timeoutMs) throws IOException {
        UsbDeviceConnection c = conn;
        if (c == null) throw new IOException("USB cerrado");
        int n = c.bulkTransfer(epIn, buf, buf.length, Math.max(1, timeoutMs));
        return Math.max(0, n);
    }

    /** Lectura continua en un hilo propio (modo enlace). */
    public void startReading(final DataListener listener) {
        if (reading) return;
        reading = true;
        reader = new Thread(new Runnable() {
            @Override
            public void run() {
                byte[] buf = new byte[4096];
                while (reading) {
                    UsbDeviceConnection c = conn;
                    if (c == null) break;
                    int n = c.bulkTransfer(epIn, buf, buf.length, 100);
                    if (n > 0 && reading) listener.onData(Arrays.copyOf(buf, n));
                }
            }
        }, "xrtbox-usb-read");
        reader.start();
    }

    public void stopReading() {
        reading = false;
        Thread t = reader;
        reader = null;
        if (t != null && t != Thread.currentThread()) {
            try {
                t.join(400);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }
}
