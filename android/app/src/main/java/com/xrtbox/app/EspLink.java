package com.xrtbox.app;

import android.Manifest;
import android.app.PendingIntent;
import android.bluetooth.BluetoothAdapter;
import android.bluetooth.BluetoothDevice;
import android.bluetooth.BluetoothGatt;
import android.bluetooth.BluetoothGattCallback;
import android.bluetooth.BluetoothGattCharacteristic;
import android.bluetooth.BluetoothGattDescriptor;
import android.bluetooth.BluetoothGattService;
import android.bluetooth.BluetoothManager;
import android.bluetooth.BluetoothProfile;
import android.bluetooth.BluetoothStatusCodes;
import android.bluetooth.le.BluetoothLeScanner;
import android.bluetooth.le.ScanCallback;
import android.bluetooth.le.ScanRecord;
import android.bluetooth.le.ScanResult;
import android.bluetooth.le.ScanSettings;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.text.SpannableStringBuilder;
import android.text.Spanned;
import android.text.style.ForegroundColorSpan;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.zip.CRC32;

/**
 * Enlace con el ESP32 que lleva el firmware X-RTbox, por Bluetooth LE o por
 * cable USB-C (OTG). Tambien instala el firmware por USB.
 * Todo el estado se maneja en el hilo principal.
 *
 * Protocolo (texto, el primer caracter indica el tipo; por USB va en tramas, ver UsbFraming):
 *   movil -> ESP32:  B<len>|<crc32 hex>|<nombre>   D<datos>   E   R   S   I   M<mtu>   W<texto>
 *   ESP32 -> movil:  L<salida del programa>   K<mensaje ok>   !<error>   T<estado>|<programa>|<memoria>
 */
public class EspLink {

    public static final UUID SERVICE = UUID.fromString("a7c40001-ee5b-4fd0-9052-1429fba680b4");
    public static final UUID RX = UUID.fromString("a7c40002-ee5b-4fd0-9052-1429fba680b4");
    public static final UUID TX = UUID.fromString("a7c40003-ee5b-4fd0-9052-1429fba680b4");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    public static final String NAME_PREFIX = "X-RTbox";
    public static final int MAX_PROGRAM = 32 * 1024;
    private static final int MAX_WIRE_NAME = 46;

    public static final int LINK_IDLE = 0;
    public static final int LINK_CONNECTING = 1;
    public static final int LINK_READY = 2;

    public static final int TRANSPORT_NONE = 0;
    public static final int TRANSPORT_BLE = 1;
    public static final int TRANSPORT_USB = 2;
    private static final int USB_CHUNK = 180;
    private static final String ACTION_USB_PERMISSION = "com.xrtbox.app.USB_PERMISSION";

    public static final int KIND_OUT = 0;
    public static final int KIND_OK = 1;
    public static final int KIND_ERR = 2;
    public static final int KIND_SYS = 3;
    public static final int KIND_IN = 4;

    public interface Listener {
        void onLinkChanged();

        void onBusyChanged();

        void onConsoleChanged();

        void onDevicesChanged();

        /** Resultado de cargar un programa, o error de ejecucion (ok = false). */
        void onProgramResult(boolean ok, String programName, String message);

        void onLinkMessage(String message);

        /** Fin de la instalacion del firmware por USB. */
        void onFlashFinished(boolean ok, String message);

        /** El ESP32 tiene un firmware distinto del que trae la app ("" = antiguo, sin version). */
        void onFirmwareOutdated(String remoteVersion);

        /** Nueva captura del osciloscopio. */
        void onScopeData(ScopeFrame frame);
    }

    /** Captura del osciloscopio: muestras 0..255 (0..3,3 V) de cada pin activo. */
    public static final class ScopeFrame {
        public int seq;
        public int windowUs;
        public int n;
        public int[] pins = new int[0];
        public boolean[] analog = new boolean[0];
        public byte[][] data = new byte[0][];
        public int trigPin = -1;
        public boolean triggered;
    }

    public interface UsbPermissionCallback {
        void onResult(boolean granted);
    }

    public static class Found {
        public final BluetoothDevice device;
        public final String name;
        public int rssi;

        Found(BluetoothDevice d, String n, int r) {
            device = d;
            name = n;
            rssi = r;
        }
    }

    private static EspLink instance;

    public static synchronized EspLink get(Context ctx) {
        if (instance == null) instance = new EspLink(ctx.getApplicationContext());
        return instance;
    }

    private final Context ctx;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final BluetoothAdapter adapter;
    private final SharedPreferences prefs;
    private final List<Listener> listeners = new ArrayList<>();
    private final List<Found> found = new ArrayList<>();

    // conexion
    private int link = LINK_IDLE;
    private int transport = TRANSPORT_NONE;
    private boolean scanning = false;
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothDevice device;
    private BluetoothGattCharacteristic rxChar, txChar;
    private int mtu = 23;
    private boolean mtuDone;
    private int attempts;
    private boolean silentConnect;
    private String deviceName = "";

    // cola de escritura
    private final ArrayDeque<byte[]> queue = new ArrayDeque<>();
    private boolean writing;
    private int writeRetries;

    // estado del ESP32
    private String remoteState = "NONE";
    private String remoteProgram = "";
    private String remoteFw = null;       // version del firmware del ESP32 (null = aun no se sabe)
    // osciloscopio
    private static final int SCOPE_POINTS = 250;
    private boolean scopeOn = false;
    private int scopeDivUs = 1000;
    private int scopeTrig = -1;
    private ScopeFrame scopeBuild = null;
    private int scopeGot = 0;
    private final Runnable scopeRequestRunnable = new Runnable() {
        @Override
        public void run() {
            requestScope();
        }
    };
    private boolean fwWarned = false;
    private String bundledFw = null;
    private int remoteHeap = 0;

    // envio de programas
    private boolean sending;
    private boolean gotReceived;
    private String sendingName = "";
    private String lastError;
    private boolean commandPending;
    private String commandText = "";

    // consola
    private final SpannableStringBuilder console = new SpannableStringBuilder();
    private byte[] utfPending = new byte[0];
    private boolean atLineStart = true;
    private boolean consolePosted;

    private String busyText;

    // USB
    private final UsbManager usbManager;
    private final ExecutorService usbWriter = Executors.newSingleThreadExecutor();
    private UsbSerial usb;
    private UsbDevice usbDevice;
    private UsbFraming.Parser usbParser;
    private int helloTries;
    private UsbPermissionCallback pendingPermission;

    // instalacion de firmware
    private boolean flashing;
    private int flashPercent;
    private String flashStep = "";

    private EspLink(Context c) {
        ctx = c;
        BluetoothManager bm = (BluetoothManager) c.getSystemService(Context.BLUETOOTH_SERVICE);
        adapter = bm != null ? bm.getAdapter() : null;
        prefs = c.getSharedPreferences("xrtbox", Context.MODE_PRIVATE);
        usbManager = (UsbManager) c.getSystemService(Context.USB_SERVICE);
        IntentFilter f = new IntentFilter();
        f.addAction(ACTION_USB_PERMISSION);
        f.addAction(UsbManager.ACTION_USB_DEVICE_DETACHED);
        if (Build.VERSION.SDK_INT >= 33) c.registerReceiver(usbReceiver, f, Context.RECEIVER_NOT_EXPORTED);
        else c.registerReceiver(usbReceiver, f);
    }

    // ------------------------------------------------------------------
    //  Oyentes
    // ------------------------------------------------------------------
    public void addListener(Listener l) {
        if (!listeners.contains(l)) listeners.add(l);
    }

    public void removeListener(Listener l) {
        listeners.remove(l);
    }

    private void notifyLink() {
        for (Listener l : new ArrayList<>(listeners)) l.onLinkChanged();
    }

    private void notifyDevices() {
        for (Listener l : new ArrayList<>(listeners)) l.onDevicesChanged();
    }

    private void notifyMessage(String m) {
        for (Listener l : new ArrayList<>(listeners)) l.onLinkMessage(m);
    }

    private void notifyResult(boolean ok, String name, String msg) {
        for (Listener l : new ArrayList<>(listeners)) l.onProgramResult(ok, name, msg);
    }

    private void notifyFlash(boolean ok, String msg) {
        for (Listener l : new ArrayList<>(listeners)) l.onFlashFinished(ok, msg);
    }

    private final Runnable consoleNotifier = new Runnable() {
        @Override
        public void run() {
            consolePosted = false;
            for (Listener l : new ArrayList<>(listeners)) l.onConsoleChanged();
        }
    };

    private void notifyConsole() {
        if (consolePosted) return;
        consolePosted = true;
        main.postDelayed(consoleNotifier, 80);
    }

    // ------------------------------------------------------------------
    //  Consultas
    // ------------------------------------------------------------------
    public boolean isSupported() {
        return adapter != null && ctx.getPackageManager().hasSystemFeature(PackageManager.FEATURE_BLUETOOTH_LE);
    }

    public boolean isEnabled() {
        try {
            return adapter != null && adapter.isEnabled();
        } catch (SecurityException e) {
            return false;
        }
    }

    public boolean isReady() {
        return link == LINK_READY;
    }

    public int getLinkState() {
        return link;
    }

    public int getTransport() {
        return transport;
    }

    public boolean isFlashing() {
        return flashing;
    }

    public String getFlashStep() {
        return flashStep;
    }

    public int getFlashPercent() {
        return flashPercent;
    }

    public boolean isScanning() {
        return scanning;
    }

    public String getDeviceName() {
        return deviceName;
    }

    public String getRemoteState() {
        return remoteState;
    }

    public String getRemoteProgram() {
        return remoteProgram;
    }

    public int getRemoteHeap() {
        return remoteHeap;
    }

    public String getBusyText() {
        return busyText;
    }

    public List<Found> getFound() {
        return found;
    }

    public CharSequence getConsole() {
        return console;
    }

    public String getLastAddress() {
        return prefs.getString("last_address", null);
    }

    public String getLastName() {
        return prefs.getString("last_name", "");
    }

    public void clearConsole() {
        console.clear();
        console.clearSpans();
        atLineStart = true;
        notifyConsole();
    }

    /** Nombre tal y como viaja al ESP32 (max. 46 bytes UTF-8, sin '|'). */
    public static String wireName(String name) {
        String s = name.replace('|', '_');
        while (s.getBytes(StandardCharsets.UTF_8).length > MAX_WIRE_NAME) s = s.substring(0, s.length() - 1);
        return s;
    }

    private boolean hasPerm(String p) {
        return ctx.checkSelfPermission(p) == PackageManager.PERMISSION_GRANTED;
    }

    public boolean hasConnectPermission() {
        return Build.VERSION.SDK_INT < 31 || hasPerm(Manifest.permission.BLUETOOTH_CONNECT);
    }

    // ------------------------------------------------------------------
    //  Indicador de "ocupado" (hace girar el logo)
    // ------------------------------------------------------------------
    private void computeBusy() {
        String b = null;
        if (flashing) b = "INSTALANDO FIRMWARE " + flashPercent + "%";
        else if (sending) b = "CARGANDO PROGRAMA";
        else if (link == LINK_CONNECTING) b = transport == TRANSPORT_USB ? "CONECTANDO POR USB" : (silentConnect ? "RECONECTANDO" : "CONECTANDO");
        else if (commandPending) b = commandText;
        else if (scanning) b = "BUSCANDO ESP32";
        boolean changed = b == null ? busyText != null : !b.equals(busyText);
        busyText = b;
        if (changed) for (Listener l : new ArrayList<>(listeners)) l.onBusyChanged();
    }

    // ------------------------------------------------------------------
    //  Busqueda
    // ------------------------------------------------------------------
    private final Runnable scanTimeout = new Runnable() {
        @Override
        public void run() {
            stopScan();
        }
    };

    private final ScanCallback scanCb = new ScanCallback() {
        @Override
        public void onScanResult(int callbackType, final ScanResult result) {
            main.post(new Runnable() {
                @Override
                public void run() {
                    handleScan(result);
                }
            });
        }

        @Override
        public void onBatchScanResults(final List<ScanResult> results) {
            main.post(new Runnable() {
                @Override
                public void run() {
                    for (ScanResult r : results) handleScan(r);
                }
            });
        }

        @Override
        public void onScanFailed(final int errorCode) {
            main.post(new Runnable() {
                @Override
                public void run() {
                    scanning = false;
                    main.removeCallbacks(scanTimeout);
                    computeBusy();
                    notifyLink();
                    notifyMessage("No se pudo buscar dispositivos (código " + errorCode + "). Prueba a apagar y encender el Bluetooth.");
                }
            });
        }
    };

    private void handleScan(ScanResult r) {
        if (!scanning || r == null) return;
        BluetoothDevice d = r.getDevice();
        ScanRecord rec = r.getScanRecord();
        String name = rec != null ? rec.getDeviceName() : null;
        if (name == null) {
            try {
                name = d.getName();
            } catch (SecurityException e) {
                name = null;
            }
        }
        boolean match = name != null && name.startsWith(NAME_PREFIX);
        if (!match && rec != null && rec.getServiceUuids() != null) {
            match = rec.getServiceUuids().contains(new ParcelUuid(SERVICE));
        }
        if (!match) return;
        if (name == null || name.isEmpty()) name = NAME_PREFIX;
        for (Found f : found) {
            if (f.device.getAddress().equals(d.getAddress())) {
                f.rssi = r.getRssi();
                notifyDevices();
                return;
            }
        }
        found.add(new Found(d, name, r.getRssi()));
        notifyDevices();
    }

    public boolean startScan() {
        if (!isEnabled()) return false;
        if (scanning) return true;
        scanner = adapter.getBluetoothLeScanner();
        if (scanner == null) return false;
        found.clear();
        notifyDevices();
        ScanSettings st = new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build();
        try {
            scanner.startScan(null, st, scanCb);
        } catch (SecurityException e) {
            notifyMessage("Falta el permiso de Bluetooth");
            return false;
        } catch (IllegalStateException e) {
            notifyMessage("El Bluetooth está apagado");
            return false;
        }
        scanning = true;
        main.postDelayed(scanTimeout, 12000);
        computeBusy();
        notifyLink();
        return true;
    }

    public void stopScan() {
        if (!scanning) return;
        scanning = false;
        main.removeCallbacks(scanTimeout);
        try {
            if (scanner != null && isEnabled()) scanner.stopScan(scanCb);
        } catch (SecurityException | IllegalStateException e) {
            // ignorar
        }
        computeBusy();
        notifyLink();
    }

    // ------------------------------------------------------------------
    //  Conexion
    // ------------------------------------------------------------------
    private final Runnable connectTimeout = new Runnable() {
        @Override
        public void run() {
            if (link != LINK_CONNECTING) return;
            closeGatt();
            link = LINK_IDLE;
            computeBusy();
            notifyLink();
            if (!silentConnect) notifyMessage("No se pudo conectar: el ESP32 no responde. Comprueba que está encendido y cerca.");
        }
    };

    private final Runnable mtuFallback = new Runnable() {
        @Override
        public void run() {
            if (gatt != null && !mtuDone && link == LINK_CONNECTING) {
                mtuDone = true;
                discover(gatt);
            }
        }
    };

    public void connect(BluetoothDevice d, String name, boolean silent) {
        if (flashing) return;
        stopScan();
        if (gatt != null) closeGatt();
        if (usb != null) closeUsb();
        transport = TRANSPORT_BLE;
        device = d;
        deviceName = name == null || name.isEmpty() ? NAME_PREFIX : name;
        silentConnect = silent;
        attempts = 0;
        link = LINK_CONNECTING;
        remoteState = "NONE";
        remoteProgram = "";
        remoteFw = null;
        fwWarned = false;
        main.removeCallbacks(connectTimeout);
        main.postDelayed(connectTimeout, 15000);
        if (!silent) appendLine("» Conectando con " + deviceName + "...", KIND_SYS);
        computeBusy();
        notifyLink();
        openGatt();
    }

    public boolean connectAddress(String address, String name, boolean silent) {
        if (!isEnabled() || address == null) return false;
        try {
            BluetoothDevice d = adapter.getRemoteDevice(address);
            connect(d, name, silent);
            return true;
        } catch (IllegalArgumentException e) {
            return false;
        }
    }

    private void openGatt() {
        mtuDone = false;
        mtu = 23;
        try {
            gatt = device.connectGatt(ctx, false, gattCb, BluetoothDevice.TRANSPORT_LE);
        } catch (SecurityException e) {
            gatt = null;
        }
        if (gatt == null) {
            link = LINK_IDLE;
            main.removeCallbacks(connectTimeout);
            computeBusy();
            notifyLink();
            notifyMessage("No se pudo iniciar la conexión Bluetooth (permiso denegado)");
        }
    }

    public void disconnect() {
        if (link == LINK_IDLE && gatt == null && usb == null) return;
        boolean wasReady = link == LINK_READY;
        closeGatt();
        closeUsb();
        link = LINK_IDLE;
        main.removeCallbacks(connectTimeout);
        failSending("Desconectado");
        if (wasReady) appendLine("» Desconectado de " + deviceName, KIND_SYS);
        computeBusy();
        notifyLink();
    }

    private void closeGatt() {
        main.removeCallbacks(mtuFallback);
        main.removeCallbacks(writeWatchdog);
        if (gatt != null) {
            try {
                gatt.disconnect();
                gatt.close();
            } catch (SecurityException e) {
                // ignorar
            }
        }
        gatt = null;
        rxChar = null;
        txChar = null;
        queue.clear();
        writing = false;
        commandPending = false;
        main.removeCallbacks(commandTimeout);
        remoteState = "NONE";
        remoteProgram = "";
        remoteFw = null;
        fwWarned = false;
    }

    private final BluetoothGattCallback gattCb = new BluetoothGattCallback() {
        @Override
        public void onConnectionStateChange(final BluetoothGatt g, final int status, final int newState) {
            main.post(new Runnable() {
                @Override
                public void run() {
                    handleConnection(g, status, newState);
                }
            });
        }

        @Override
        public void onMtuChanged(final BluetoothGatt g, final int newMtu, final int status) {
            main.post(new Runnable() {
                @Override
                public void run() {
                    if (g != gatt || mtuDone) return;
                    mtuDone = true;
                    main.removeCallbacks(mtuFallback);
                    mtu = status == BluetoothGatt.GATT_SUCCESS ? newMtu : 23;
                    discover(g);
                }
            });
        }

        @Override
        public void onServicesDiscovered(final BluetoothGatt g, final int status) {
            main.post(new Runnable() {
                @Override
                public void run() {
                    handleServices(g, status);
                }
            });
        }

        @Override
        public void onDescriptorWrite(final BluetoothGatt g, BluetoothGattDescriptor d, int status) {
            main.post(new Runnable() {
                @Override
                public void run() {
                    if (g == gatt && link == LINK_CONNECTING) onReady();
                }
            });
        }

        @Override
        public void onCharacteristicWrite(final BluetoothGatt g, BluetoothGattCharacteristic c, final int status) {
            main.post(new Runnable() {
                @Override
                public void run() {
                    if (g == gatt) handleWritten(status);
                }
            });
        }

        @SuppressWarnings("deprecation")
        @Override
        public void onCharacteristicChanged(final BluetoothGatt g, BluetoothGattCharacteristic c) {
            byte[] v = c.getValue();
            if (v == null) return;
            final byte[] copy = v.clone();
            main.post(new Runnable() {
                @Override
                public void run() {
                    if (g == gatt) handleFrame(copy);
                }
            });
        }

        @Override
        public void onCharacteristicChanged(final BluetoothGatt g, BluetoothGattCharacteristic c, byte[] value) {
            final byte[] copy = value.clone();
            main.post(new Runnable() {
                @Override
                public void run() {
                    if (g == gatt) handleFrame(copy);
                }
            });
        }
    };

    private void handleConnection(final BluetoothGatt g, int status, int newState) {
        if (g != gatt) {
            try {
                g.close();
            } catch (SecurityException e) {
                // ignorar
            }
            return;
        }
        if (newState == BluetoothProfile.STATE_CONNECTED && status == BluetoothGatt.GATT_SUCCESS) {
            main.postDelayed(new Runnable() {
                @Override
                public void run() {
                    if (g != gatt) return;
                    boolean asked;
                    try {
                        asked = g.requestMtu(517);
                    } catch (SecurityException e) {
                        asked = false;
                    }
                    if (asked) main.postDelayed(mtuFallback, 2500);
                    else {
                        mtuDone = true;
                        discover(g);
                    }
                }
            }, 250);
            return;
        }
        if (newState == BluetoothProfile.STATE_DISCONNECTED || status != BluetoothGatt.GATT_SUCCESS) {
            boolean wasReady = link == LINK_READY;
            boolean wasConnecting = link == LINK_CONNECTING;
            closeGatt();
            if (wasConnecting && attempts < 2) {
                // el error 133 es muy comun en Android: se reintenta
                attempts++;
                main.postDelayed(new Runnable() {
                    @Override
                    public void run() {
                        if (link == LINK_CONNECTING && gatt == null && device != null) openGatt();
                    }
                }, 700);
                return;
            }
            link = LINK_IDLE;
            main.removeCallbacks(connectTimeout);
            failSending("Se perdió la conexión");
            if (wasReady) {
                appendLine("» Se perdió la conexión con " + deviceName, KIND_ERR);
                notifyMessage("Se perdió la conexión con el ESP32");
            } else if (wasConnecting && !silentConnect) {
                notifyMessage("No se pudo conectar con " + deviceName + " (código " + status + ")");
            }
            computeBusy();
            notifyLink();
        }
    }

    private void discover(BluetoothGatt g) {
        boolean ok;
        try {
            ok = g.discoverServices();
        } catch (SecurityException e) {
            ok = false;
        }
        if (!ok) failConnect("No se pudieron leer los servicios del dispositivo");
    }

    private void failConnect(String msg) {
        closeGatt();
        link = LINK_IDLE;
        main.removeCallbacks(connectTimeout);
        computeBusy();
        notifyLink();
        if (!silentConnect) notifyMessage(msg);
    }

    @SuppressWarnings("deprecation")
    private void handleServices(BluetoothGatt g, int status) {
        if (g != gatt || link != LINK_CONNECTING) return;
        if (status != BluetoothGatt.GATT_SUCCESS) {
            failConnect("No se pudieron leer los servicios del dispositivo");
            return;
        }
        BluetoothGattService svc = g.getService(SERVICE);
        if (svc == null) {
            failConnect("Ese dispositivo no tiene el firmware X-RTbox");
            return;
        }
        rxChar = svc.getCharacteristic(RX);
        txChar = svc.getCharacteristic(TX);
        if (rxChar == null || txChar == null) {
            failConnect("El firmware del ESP32 no es compatible con esta app");
            return;
        }
        try {
            g.setCharacteristicNotification(txChar, true);
            BluetoothGattDescriptor d = txChar.getDescriptor(CCCD);
            boolean started = false;
            if (d != null) {
                if (Build.VERSION.SDK_INT >= 33) {
                    started = g.writeDescriptor(d, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS;
                } else {
                    d.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE);
                    started = g.writeDescriptor(d);
                }
            }
            if (!started) onReady();
        } catch (SecurityException e) {
            failConnect("Falta el permiso de Bluetooth");
        }
    }

    private void onReady() {
        if (link == LINK_READY) return;
        link = LINK_READY;
        attempts = 0;
        main.removeCallbacks(connectTimeout);
        if (transport == TRANSPORT_USB) {
            appendLine("» Enlazado por USB-C (" + deviceName + ")", KIND_OK);
            main.removeCallbacks(usbHello);
            main.postDelayed(usbPing, 4000);
            computeBusy();
            notifyLink();
            return;
        }
        prefs.edit().putString("last_address", device.getAddress()).putString("last_name", deviceName).apply();
        try {
            gatt.requestConnectionPriority(BluetoothGatt.CONNECTION_PRIORITY_HIGH);
        } catch (SecurityException e) {
            // ignorar
        }
        appendLine("» Enlazado con " + deviceName + " (Bluetooth)", KIND_OK);
        enqueue(("M" + mtu).getBytes(StandardCharsets.UTF_8));
        enqueue("I".getBytes(StandardCharsets.UTF_8));
        computeBusy();
        notifyLink();
    }

    // ------------------------------------------------------------------
    //  Escritura
    // ------------------------------------------------------------------
    private final Runnable writeWatchdog = new Runnable() {
        @Override
        public void run() {
            if (!writing) return;
            writing = false;
            queue.clear();
            failSending("El ESP32 dejó de responder");
        }
    };

    private final Runnable pumpRunnable = new Runnable() {
        @Override
        public void run() {
            pump();
        }
    };

    private void enqueue(byte[] data) {
        queue.add(data);
        pump();
    }

    private void pump() {
        if (writing || queue.isEmpty()) return;
        if (transport == TRANSPORT_USB) {
            pumpUsb();
            return;
        }
        if (gatt == null || rxChar == null) return;
        byte[] d = queue.peek();
        if (writeChar(d)) {
            writing = true;
            writeRetries = 0;
            main.postDelayed(writeWatchdog, 6000);
        } else if (++writeRetries > 25) {
            writeRetries = 0;
            queue.clear();
            failSending("No se pudo enviar al ESP32");
        } else {
            main.postDelayed(pumpRunnable, 40);
        }
    }

    @SuppressWarnings("deprecation")
    private boolean writeChar(byte[] d) {
        try {
            if (Build.VERSION.SDK_INT >= 33) {
                return gatt.writeCharacteristic(rxChar, d, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT)
                        == BluetoothStatusCodes.SUCCESS;
            }
            rxChar.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
            rxChar.setValue(d);
            return gatt.writeCharacteristic(rxChar);
        } catch (SecurityException e) {
            return false;
        }
    }

    private void handleWritten(int status) {
        main.removeCallbacks(writeWatchdog);
        writing = false;
        if (status != BluetoothGatt.GATT_SUCCESS) {
            queue.clear();
            failSending("Error al enviar (código " + status + ")");
            return;
        }
        queue.poll();
        pump();
    }

    // ------------------------------------------------------------------
    //  Programas y ordenes
    // ------------------------------------------------------------------
    private final Runnable sendTimeout = new Runnable() {
        @Override
        public void run() {
            if (sending) finishSending(false, "El ESP32 no respondió a tiempo");
        }
    };

    private final Runnable commandTimeout = new Runnable() {
        @Override
        public void run() {
            commandPending = false;
            computeBusy();
        }
    };

    /** Envia un programa al ESP32. Devuelve false si no hay conexion. */
    public boolean sendProgram(String name, String code) {
        if (link != LINK_READY) return false;
        if (sending) {
            notifyMessage("Ya se está cargando un programa, espera un momento");
            return true;
        }
        String clean = code.replace("\r\n", "\n").replace('\r', '\n').replace("\u0000", "");
        byte[] data = clean.getBytes(StandardCharsets.UTF_8);
        if (data.length == 0) {
            notifyResult(false, name, "El programa está vacío");
            return true;
        }
        if (data.length > MAX_PROGRAM) {
            notifyResult(false, name, "El programa es demasiado grande (" + data.length + " bytes, máximo " + MAX_PROGRAM + ")");
            return true;
        }
        CRC32 crc = new CRC32();
        crc.update(data);
        String wn = wireName(name);
        queue.add(("B" + data.length + "|" + Long.toHexString(crc.getValue()) + "|" + wn).getBytes(StandardCharsets.UTF_8));
        int chunk = transport == TRANSPORT_USB ? USB_CHUNK : Math.max(19, Math.min(mtu - 4, 500));
        for (int off = 0; off < data.length; off += chunk) {
            int n = Math.min(chunk, data.length - off);
            byte[] f = new byte[n + 1];
            f[0] = 'D';
            System.arraycopy(data, off, f, 1, n);
            queue.add(f);
        }
        queue.add(new byte[]{'E'});
        sending = true;
        gotReceived = false;
        sendingName = name;
        lastError = null;
        main.removeCallbacks(sendTimeout);
        main.postDelayed(sendTimeout, 25000 + data.length / 10);
        appendLine("» Cargando '" + name + "' (" + data.length + " bytes)...", KIND_SYS);
        computeBusy();
        pump();
        return true;
    }

    private void failSending(String msg) {
        if (sending) finishSending(false, msg);
    }

    private void finishSending(boolean ok, String msg) {
        if (!sending) return;
        sending = false;
        main.removeCallbacks(sendTimeout);
        computeBusy();
        notifyResult(ok, sendingName, msg);
    }

    private void command(String cmd, String busy) {
        if (link != LINK_READY) return;
        commandPending = true;
        commandText = busy;
        main.removeCallbacks(commandTimeout);
        main.postDelayed(commandTimeout, 4000);
        computeBusy();
        enqueue(cmd.getBytes(StandardCharsets.UTF_8));
    }

    public void sendStop() {
        command("S", "DETENIENDO");
    }

    public void sendRun() {
        command("R", "REINICIANDO");
    }

    public void requestInfo() {
        if (link == LINK_READY) enqueue("I".getBytes(StandardCharsets.UTF_8));
    }

    /** Envia texto a la entrada Serial del programa (Serial.read / readStringUntil). */
    public void sendInput(String text) {
        if (link != LINK_READY) return;
        byte[] data = (text + "\n").getBytes(StandardCharsets.UTF_8);
        int chunk = transport == TRANSPORT_USB ? USB_CHUNK : Math.max(19, Math.min(mtu - 4, 500));
        for (int off = 0; off < data.length; off += chunk) {
            int n = Math.min(chunk, data.length - off);
            byte[] f = new byte[n + 1];
            f[0] = 'W';
            System.arraycopy(data, off, f, 1, n);
            enqueue(f);
        }
        appendLine("> " + text, KIND_IN);
    }

    // ------------------------------------------------------------------
    //  USB-C (OTG)
    // ------------------------------------------------------------------
    private final BroadcastReceiver usbReceiver = new BroadcastReceiver() {
        @SuppressWarnings("deprecation")
        @Override
        public void onReceive(Context c, Intent intent) {
            String action = intent.getAction();
            if (ACTION_USB_PERMISSION.equals(action)) {
                boolean granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false);
                UsbPermissionCallback cb = pendingPermission;
                pendingPermission = null;
                if (cb != null) cb.onResult(granted);
            } else if (UsbManager.ACTION_USB_DEVICE_DETACHED.equals(action)) {
                UsbDevice d = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE);
                if (d != null && usbDevice != null && d.getDeviceName().equals(usbDevice.getDeviceName()) && usb != null) {
                    boolean wasReady = link == LINK_READY;
                    closeUsb();
                    link = LINK_IDLE;
                    failSending("Se desconecto el cable USB");
                    if (wasReady) {
                        appendLine("» Cable USB desconectado", KIND_ERR);
                        notifyMessage("Se desconecto el cable USB del ESP32");
                    }
                    computeBusy();
                    notifyLink();
                }
            }
        }
    };

    /** Dispositivos USB conectados que parecen un ESP32 (puerto serie compatible). */
    public List<UsbDevice> getUsbDevices() {
        List<UsbDevice> out = new ArrayList<>();
        if (usbManager == null) return out;
        for (UsbDevice d : usbManager.getDeviceList().values()) {
            if (UsbSerial.isSupported(d)) out.add(d);
        }
        return out;
    }

    public boolean hasUsbPermission(UsbDevice d) {
        return usbManager != null && usbManager.hasPermission(d);
    }

    public void requestUsbPermission(UsbDevice d, UsbPermissionCallback cb) {
        if (usbManager == null) {
            cb.onResult(false);
            return;
        }
        if (usbManager.hasPermission(d)) {
            cb.onResult(true);
            return;
        }
        pendingPermission = cb;
        Intent i = new Intent(ACTION_USB_PERMISSION);
        i.setPackage(ctx.getPackageName());
        int flags = Build.VERSION.SDK_INT >= 31 ? PendingIntent.FLAG_MUTABLE : 0;
        usbManager.requestPermission(d, PendingIntent.getBroadcast(ctx, 0, i, flags));
    }

    private final Runnable usbHello = new Runnable() {
        @Override
        public void run() {
            if (transport != TRANSPORT_USB || link != LINK_CONNECTING || usb == null) return;
            if (helloTries++ >= 7) {
                closeUsb();
                link = LINK_IDLE;
                computeBusy();
                notifyLink();
                appendLine("» El ESP32 del USB no responde", KIND_ERR);
                notifyMessage("No se detecta el firmware X-RTbox en el ESP32 del USB. Pulsa INSTALAR FIRMWARE BLUETOOTH para instalarlo.");
                return;
            }
            writeUsbRaw(UsbFraming.encode('I', new byte[0]));
            main.postDelayed(this, 900);
        }
    };

    private final Runnable usbPing = new Runnable() {
        @Override
        public void run() {
            if (transport != TRANSPORT_USB || link != LINK_READY) return;
            if (queue.isEmpty() && !writing) enqueue(new byte[]{'P'});
            main.postDelayed(this, 4000);
        }
    };

    /** Enlaza con el ESP32 conectado por USB (hace falta el permiso). */
    public void connectUsb(final UsbDevice d) {
        if (flashing) return;
        stopScan();
        if (gatt != null) closeGatt();
        if (usb != null) closeUsb();
        transport = TRANSPORT_USB;
        usbDevice = d;
        deviceName = UsbSerial.describe(d);
        link = LINK_CONNECTING;
        silentConnect = false;
        remoteState = "NONE";
        remoteProgram = "";
        remoteFw = null;
        fwWarned = false;
        appendLine("» Conectando por USB-C (" + deviceName + ")...", KIND_SYS);
        computeBusy();
        notifyLink();
        final UsbSerial port = new UsbSerial(d);
        try {
            port.open(usbManager);
            port.setBaud(115200);
            port.setLines(false, false);   // sin tocar EN/IO0: no se reinicia el ESP32
        } catch (IOException e) {
            port.close();
            link = LINK_IDLE;
            computeBusy();
            notifyLink();
            notifyMessage("No se pudo abrir el USB: " + e.getMessage());
            return;
        }
        usb = port;
        utfPending = new byte[0];
        usbParser = new UsbFraming.Parser(new UsbFraming.Sink() {
            @Override
            public void onText(byte[] data) {
                appendConsole(decodeStream(data).replace("\r", ""), KIND_OUT);
            }

            @Override
            public void onFrame(char type, String text) {
                if (type == 'A') {
                    // el acuse indica que trama confirma: se ignoran los de los saludos
                    byte[] head = queue.peek();
                    char acked = text.isEmpty() ? 0 : text.charAt(0);
                    if (writing && head != null && (acked == 0 || acked == (char) (head[0] & 0xFF))) {
                        handleWritten(BluetoothGatt.GATT_SUCCESS);
                    }
                    return;
                }
                byte[] t = text.getBytes(StandardCharsets.UTF_8);
                byte[] f = new byte[t.length + 1];
                f[0] = (byte) type;
                System.arraycopy(t, 0, f, 1, t.length);
                handleFrame(f);
            }
        });
        port.startReading(new UsbSerial.DataListener() {
            @Override
            public void onData(final byte[] data) {
                main.post(new Runnable() {
                    @Override
                    public void run() {
                        if (usb == port && usbParser != null) usbParser.feed(data);
                    }
                });
            }
        });
        helloTries = 0;
        main.postDelayed(usbHello, 300);
    }

    private void writeUsbRaw(final byte[] wire) {
        final UsbSerial port = usb;
        if (port == null) return;
        usbWriter.execute(new Runnable() {
            @Override
            public void run() {
                try {
                    port.write(wire);
                } catch (IOException e) {
                    main.post(new Runnable() {
                        @Override
                        public void run() {
                            if (usb == port) {
                                writing = false;
                                main.removeCallbacks(writeWatchdog);
                                queue.clear();
                                failSending("Error al enviar por USB");
                            }
                        }
                    });
                }
            }
        });
    }

    private void pumpUsb() {
        if (usb == null) return;
        byte[] d = queue.peek();
        writing = true;
        main.postDelayed(writeWatchdog, 5000);
        writeUsbRaw(UsbFraming.encode((char) (d[0] & 0xFF), Arrays.copyOfRange(d, 1, d.length)));
    }

    private void closeUsb() {
        main.removeCallbacks(usbHello);
        main.removeCallbacks(usbPing);
        main.removeCallbacks(writeWatchdog);
        if (usb != null) {
            final UsbSerial port = usb;
            usb = null;
            usbParser = null;
            port.stopReading();
            port.close();
        }
        if (transport == TRANSPORT_USB) {
            queue.clear();
            writing = false;
            commandPending = false;
            main.removeCallbacks(commandTimeout);
            remoteState = "NONE";
            remoteProgram = "";
            remoteFw = null;
            fwWarned = false;
        }
    }

    // ------------------------------------------------------------------
    //  Instalar el firmware X-RTbox por USB
    // ------------------------------------------------------------------
    /** Version del firmware que trae la app (assets/firmware/version.txt), o "" si no la tiene. */
    public String getBundledFirmwareVersion() {
        if (bundledFw != null) return bundledFw;
        bundledFw = "";
        try (InputStream in = ctx.getAssets().open("firmware/version.txt")) {
            byte[] b = new byte[64];
            int n = in.read(b);
            if (n > 0) bundledFw = new String(b, 0, n, StandardCharsets.UTF_8).trim();
        } catch (IOException e) {
            // app compilada sin version
        }
        return bundledFw;
    }

    /** Version del firmware del ESP32 enlazado: null si aun no se sabe, "" si es antiguo (no la envia). */
    public String getRemoteFirmwareVersion() {
        return remoteFw;
    }

    /** true si el ESP32 enlazado tiene un firmware distinto del que trae la app. */
    public boolean isFirmwareOutdated() {
        String b = getBundledFirmwareVersion();
        return link == LINK_READY && remoteFw != null && !b.isEmpty() && !b.equals(remoteFw);
    }

    /** El ESP32 enlazado por USB (o null). */
    public UsbDevice getConnectedUsbDevice() {
        return transport == TRANSPORT_USB ? usbDevice : null;
    }

    // ------------------------------------------------------------------
    //  Osciloscopio: el ESP32 mide sus pines de salida y manda las muestras
    // ------------------------------------------------------------------
    /** Empieza a pedir capturas (us por division, pin de disparo o -1). */
    public void startScope(int divUs, int trigPin) {
        scopeOn = true;
        scopeDivUs = divUs;
        scopeTrig = trigPin;
        requestScope();
    }

    public void stopScope() {
        scopeOn = false;
        scopeBuild = null;
        main.removeCallbacks(scopeRequestRunnable);
    }

    public boolean isScopeOn() {
        return scopeOn;
    }

    public void setScopeTimebase(int divUs) {
        scopeDivUs = divUs;
        if (scopeOn) requestScope();
    }

    public void setScopeTrigger(int pin) {
        scopeTrig = pin;
        if (scopeOn) requestScope();
    }

    private void requestScope() {
        main.removeCallbacks(scopeRequestRunnable);
        if (!scopeOn) return;
        if (link != LINK_READY || flashing || sending) {
            main.postDelayed(scopeRequestRunnable, 700);
            return;
        }
        enqueue(("O" + scopeDivUs + "|" + SCOPE_POINTS + "|" + scopeTrig).getBytes(StandardCharsets.UTF_8));
        // si no llega la captura entera, se vuelve a pedir
        main.postDelayed(scopeRequestRunnable, scopeDivUs * 10L / 1000L + 3000L);
    }

    private void scopeHeader(String h) {
        if (!scopeOn) return;
        String[] p = h.split("\\|", -1);
        if (p.length < 6) return;
        try {
            ScopeFrame f = new ScopeFrame();
            f.seq = Integer.parseInt(p[0].trim()) & 0xFF;
            f.windowUs = Integer.parseInt(p[1].trim());
            f.n = Integer.parseInt(p[2].trim());
            String[] ps = p[3].isEmpty() ? new String[0] : p[3].split(",");
            f.pins = new int[ps.length];
            f.analog = new boolean[ps.length];
            f.data = new byte[ps.length][f.n];
            for (int k = 0; k < ps.length; k++) {
                String q = ps[k].trim();
                f.analog[k] = q.endsWith("a");
                f.pins[k] = Integer.parseInt(q.substring(0, q.length() - 1));
            }
            f.trigPin = Integer.parseInt(p[4].trim());
            f.triggered = "1".equals(p[5].trim());
            scopeBuild = f;
            scopeGot = 0;
            if (ps.length == 0) scopeDeliver();
        } catch (RuntimeException e) {
            scopeBuild = null;
        }
    }

    private void scopeChunk(String b64) {
        ScopeFrame f = scopeBuild;
        if (f == null) return;
        byte[] b;
        try {
            b = java.util.Base64.getDecoder().decode(b64.trim());
        } catch (IllegalArgumentException e) {
            return;
        }
        if (b.length < 5 || (b[0] & 0xFF) != f.seq) return;
        int k = b[1] & 0xFF;
        int off = ((b[2] & 0xFF) << 8) | (b[3] & 0xFF);
        int m = b.length - 4;
        if (k >= f.data.length || off + m > f.n) return;
        System.arraycopy(b, 4, f.data[k], off, m);
        scopeGot += m;
        if (scopeGot >= f.n * f.data.length) scopeDeliver();
    }

    private void scopeDeliver() {
        ScopeFrame f = scopeBuild;
        scopeBuild = null;
        if (f == null || !scopeOn) return;
        main.removeCallbacks(scopeRequestRunnable);
        for (Listener l : new ArrayList<>(listeners)) l.onScopeData(f);
        main.postDelayed(scopeRequestRunnable, transport == TRANSPORT_USB ? 40 : 150);
    }

    /** Firmwares incluidos en la app (assets/firmware/*.bin). */
    public List<String> bundledFirmwares() {
        List<String> out = new ArrayList<>();
        try {
            String[] l = ctx.getAssets().list("firmware");
            if (l != null) for (String n : l) if (n.endsWith(".bin")) out.add(n.substring(0, n.length() - 4));
        } catch (IOException e) {
            // ninguno
        }
        return out;
    }

    private byte[] loadFirmware(String asset) throws IOException {
        InputStream in;
        try {
            in = ctx.getAssets().open("firmware/" + asset + ".bin");
        } catch (IOException e) {
            return null;
        }
        try {
            ByteArrayOutputStream bo = new ByteArrayOutputStream(1 << 21);
            byte[] buf = new byte[65536];
            int n;
            while ((n = in.read(buf)) > 0) bo.write(buf, 0, n);
            return bo.toByteArray();
        } finally {
            in.close();
        }
    }

    public void flashFirmware(final UsbDevice d) {
        if (flashing) return;
        stopScan();
        if (transport == TRANSPORT_USB) disconnect();
        flashing = true;
        flashPercent = 0;
        flashStep = "Preparando...";
        appendLine("» Instalando el firmware X-RTbox por USB-C (" + UsbSerial.describe(d) + ")...", KIND_SYS);
        computeBusy();
        notifyLink();
        new Thread(new Runnable() {
            @Override
            public void run() {
                UsbSerial port = new UsbSerial(d);
                boolean ok = false;
                String msg;
                try {
                    port.open(usbManager);
                    port.setBaud(115200);
                    EspFlasher f = new EspFlasher(port, new EspFlasher.Progress() {
                        @Override
                        public void onProgress(final String message, final int percent) {
                            main.post(new Runnable() {
                                @Override
                                public void run() {
                                    flashStep = message;
                                    flashPercent = percent;
                                    computeBusy();
                                    notifyLink();
                                }
                            });
                        }
                    });
                    String chip = f.run(new EspFlasher.ImageSource() {
                        @Override
                        public byte[] load(String asset) throws IOException {
                            return loadFirmware(asset);
                        }
                    });
                    ok = true;
                    msg = "Firmware X-RTbox instalado en tu " + chip + ". Ya puedes enlazarlo por Bluetooth o por USB-C.";
                } catch (IOException e) {
                    msg = e.getMessage() != null ? e.getMessage() : "Error al instalar el firmware";
                } finally {
                    port.close();
                }
                final boolean fok = ok;
                final String fmsg = msg;
                main.post(new Runnable() {
                    @Override
                    public void run() {
                        flashing = false;
                        flashStep = "";
                        appendLine((fok ? "✔ " : "✖ ") + fmsg, fok ? KIND_OK : KIND_ERR);
                        computeBusy();
                        notifyLink();
                        notifyFlash(fok, fmsg);
                    }
                });
            }
        }, "xrtbox-flash").start();
    }

    // ------------------------------------------------------------------
    //  Mensajes del ESP32
    // ------------------------------------------------------------------
    private void handleFrame(byte[] v) {
        if (v.length == 0) return;
        char type = (char) v[0];
        byte[] payload = Arrays.copyOfRange(v, 1, v.length);
        switch (type) {
            case 'L':
                appendConsole(decodeStream(payload).replace("\r", ""), KIND_OUT);
                break;
            case 'K': {
                String msg = new String(payload, StandardCharsets.UTF_8);
                appendLine("✔ " + msg, KIND_OK);
                if (sending && msg.startsWith("Recibido")) gotReceived = true;
                break;
            }
            case '!': {
                String msg = new String(payload, StandardCharsets.UTF_8);
                appendLine("✖ " + msg, KIND_ERR);
                lastError = msg;
                if (sending) finishSending(false, msg);
                else notifyResult(false, remoteProgram, msg);
                break;
            }
            case 'O':
                scopeHeader(new String(payload, StandardCharsets.UTF_8));
                break;
            case 'Q':
                scopeChunk(new String(payload, StandardCharsets.UTF_8));
                break;
            case 'T': {
                String s = new String(payload, StandardCharsets.UTF_8);
                String[] p = s.split("\\|", -1);
                remoteState = p.length > 0 ? p[0] : "NONE";
                remoteProgram = p.length > 1 ? p[1] : "";
                try {
                    remoteHeap = p.length > 2 ? Integer.parseInt(p[2].trim()) : 0;
                } catch (NumberFormatException e) {
                    remoteHeap = 0;
                }
                remoteFw = p.length > 3 ? p[3].trim() : "";
                if (commandPending) {
                    commandPending = false;
                    main.removeCallbacks(commandTimeout);
                }
                if (transport == TRANSPORT_USB && link == LINK_CONNECTING) onReady();
                if (sending && gotReceived) {
                    if ("RUN".equals(remoteState) || "FIN".equals(remoteState)) {
                        finishSending(true, "Programa cargado");
                    } else if ("ERR".equals(remoteState)) {
                        finishSending(false, lastError != null ? lastError : "Error al cargar el programa");
                    }
                }
                if (!fwWarned && isFirmwareOutdated()) {
                    fwWarned = true;
                    final String rv = remoteFw;
                    main.post(new Runnable() {
                        @Override
                        public void run() {
                            for (Listener l : new ArrayList<>(listeners)) l.onFirmwareOutdated(rv);
                        }
                    });
                }
                computeBusy();
                notifyLink();
                break;
            }
            default:
                break;
        }
    }

    /** Decodifica UTF-8 por trozos sin romper caracteres partidos entre paquetes. */
    private String decodeStream(byte[] in) {
        byte[] all = new byte[utfPending.length + in.length];
        System.arraycopy(utfPending, 0, all, 0, utfPending.length);
        System.arraycopy(in, 0, all, utfPending.length, in.length);
        int cut = all.length;
        for (int i = Math.max(0, all.length - 3); i < all.length; i++) {
            int b = all[i] & 0xFF;
            if (b < 0xC0) continue;
            int need = b >= 0xF0 ? 4 : (b >= 0xE0 ? 3 : 2);
            if (i + need > all.length) {
                cut = i;
                break;
            }
        }
        utfPending = Arrays.copyOfRange(all, cut, all.length);
        return new String(all, 0, cut, StandardCharsets.UTF_8);
    }

    private static int colorFor(int kind) {
        switch (kind) {
            case KIND_OK:
                return 0xFF4DFF9A;
            case KIND_ERR:
                return 0xFFFF5C7A;
            case KIND_SYS:
                return 0xFFC58BFF;
            case KIND_IN:
                return 0xFF7FD4FF;
            default:
                return 0xFFFFFFFF;
        }
    }

    private void appendConsole(String text, int kind) {
        if (text.isEmpty()) return;
        int start = console.length();
        console.append(text);
        if (kind != KIND_OUT) {
            console.setSpan(new ForegroundColorSpan(colorFor(kind)), start, console.length(), Spanned.SPAN_EXCLUSIVE_EXCLUSIVE);
        }
        if (console.length() > 24000) {
            int cut = console.length() - 18000;
            int nl = -1;
            for (int i = cut; i < Math.min(console.length(), cut + 200); i++) {
                if (console.charAt(i) == '\n') {
                    nl = i + 1;
                    break;
                }
            }
            console.delete(0, nl > 0 ? nl : cut);
        }
        atLineStart = text.endsWith("\n");
        notifyConsole();
    }

    private void appendLine(String text, int kind) {
        if (!atLineStart) appendConsole("\n", KIND_OUT);
        appendConsole(text + "\n", kind);
    }
}
