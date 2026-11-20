package com.xrtbox.app;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.bluetooth.BluetoothAdapter;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.DialogInterface;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.hardware.usb.UsbDevice;
import android.hardware.usb.UsbManager;
import android.location.LocationManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.OpenableColumns;
import android.provider.Settings;
import android.text.Editable;
import android.text.InputType;
import android.text.TextWatcher;
import android.util.TypedValue;
import android.view.KeyEvent;
import android.view.LayoutInflater;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.widget.AdapterView;
import android.widget.BaseAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.IOException;
import java.io.InputStream;
import java.text.DateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

public class MainActivity extends Activity implements EspLink.Listener {

    private static final int SCREEN_HOME = 0;
    private static final int SCREEN_CREATE = 1;
    private static final int SCREEN_STORE = 2;
    private static final int SCREEN_LINK = 3;
    private static final int SCREEN_SCOPE = 4;

    // osciloscopio: escalas (1-2-5)
    private static final int[] TIME_DIVS_US = {20, 50, 100, 200, 500, 1000, 2000, 5000, 10000, 20000, 50000,
            100000, 200000, 500000, 1000000};
    private static final double[] VOLT_DIVS = {0.1, 0.2, 0.5, 1.0, 2.0, 5.0};

    private static final int REQ_PERMS = 10;
    private static final int REQ_ENABLE_BT = 11;
    private static final int REQ_IMPORT = 12;

    private static final String TEMPLATE =
            "// Parpadeo del LED integrado\n" +
            "const int LED = 2;\n" +
            "\n" +
            "void setup() {\n" +
            "  Serial.begin(115200);\n" +
            "  pinMode(LED, OUTPUT);\n" +
            "  Serial.println(\"Hola desde X-RTbox!\");\n" +
            "}\n" +
            "\n" +
            "void loop() {\n" +
            "  digitalWrite(LED, HIGH);\n" +
            "  delay(500);\n" +
            "  digitalWrite(LED, LOW);\n" +
            "  delay(500);\n" +
            "}\n";

    private EspLink ble;
    private ProgramStore store;
    private int screen = -1;

    // cabecera
    private RadiationView logo;
    private View btnScope;
    private int scopeTimeIdx = 5;      // 1 ms/div
    private int scopeVoltIdx = 3;      // 1 V/div
    private int scopeTrigPin = -1;
    private boolean scopePaused = false;
    private LinearLayout scopeList;
    private TextView scopeStatus, scopeTime, scopeVolt, scopeTrig;
    private Button btnScopeRun;
    private TextView busyLabel;
    private TextView appTitle;
    private TextView statusText;
    private FrameLayout content;

    // crear
    private EditText nameEdit;
    private CodeEditText codeEdit;
    private TextView cursorInfo;
    private String editingName;
    private int cursorLine = 1, cursorCol = 1;

    // almacen
    private ListView programList;
    private TextView emptyView;
    private ProgramAdapter programAdapter;
    private TextView consoleText;
    private ScrollView consoleScroll;
    private EditText consoleInput;
    private final List<ProgramStore.Program> programs = new ArrayList<>();

    // enlazar
    private TextView linkState;
    private TextView linkDetail;
    private Button btnScan;
    private Button btnUsb;
    private Button btnFlash;
    private Button btnDisconnect;
    private Button btnReconnect;
    private TextView deviceEmpty;
    private DeviceAdapter deviceAdapter;

    private Runnable afterPermission;
    private boolean triedAutoConnect;

    // ------------------------------------------------------------------
    //  Ciclo de vida
    // ------------------------------------------------------------------
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        logo = findViewById(R.id.logo);
        busyLabel = findViewById(R.id.busyLabel);
        appTitle = findViewById(R.id.appTitle);
        statusText = findViewById(R.id.statusText);
        content = findViewById(R.id.content);
        btnScope = findViewById(R.id.btnScope);
        btnScope.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showScreen(SCREEN_SCOPE);
            }
        });
        ble = EspLink.get(this);
        store = new ProgramStore(this);
        ble.addListener(this);
        showScreen(SCREEN_HOME);
        onBusyChanged();
        updateHeader();
        if (savedInstanceState == null) {
            if (isUsbAttachIntent(getIntent())) handleUsbIntent(getIntent());
            else autoReconnect();
        }
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        handleUsbIntent(intent);
    }

    private static boolean isUsbAttachIntent(Intent i) {
        return i != null && UsbManager.ACTION_USB_DEVICE_ATTACHED.equals(i.getAction());
    }

    /** Se ha enchufado un ESP32 por USB y Android ha abierto la app. */
    @SuppressWarnings("deprecation")
    private void handleUsbIntent(Intent i) {
        if (!isUsbAttachIntent(i)) return;
        UsbDevice d = i.getParcelableExtra(UsbManager.EXTRA_DEVICE);
        if (d == null || !UsbSerial.isSupported(d) || ble.isFlashing()) return;
        if (ble.getLinkState() != EspLink.LINK_IDLE) return;
        showScreen(SCREEN_LINK);
        askUsb(d, new UsbAction() {
            @Override
            public void run(UsbDevice dev) {
                ble.connectUsb(dev);
            }
        });
    }

    @Override
    protected void onDestroy() {
        ble.removeListener(this);
        super.onDestroy();
    }

    @Override
    protected void onResume() {
        super.onResume();
        if (screen == SCREEN_STORE) refreshPrograms();
        if (screen == SCREEN_SCOPE && !scopePaused) startScope();
    }

    @Override
    protected void onPause() {
        if (screen == SCREEN_SCOPE) ble.stopScope();
        super.onPause();
    }

    @SuppressWarnings("deprecation")
    @Override
    public void onBackPressed() {
        if (screen == SCREEN_SCOPE) {
            showScreen(SCREEN_STORE);
        } else if (screen != SCREEN_HOME) {
            if (screen == SCREEN_LINK) ble.stopScan();
            showScreen(SCREEN_HOME);
        } else {
            super.onBackPressed();
        }
    }

    // ------------------------------------------------------------------
    //  Pantallas
    // ------------------------------------------------------------------
    private int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private void showScreen(int s) {
        if (screen == SCREEN_SCOPE && s != SCREEN_SCOPE) ble.stopScope();
        screen = s;
        btnScope.setVisibility(s == SCREEN_STORE ? View.VISIBLE : View.GONE);
        content.removeAllViews();
        LayoutInflater inf = getLayoutInflater();
        boolean home = s == SCREEN_HOME;
        int size = dp(home ? 190 : 86);
        ViewGroup.LayoutParams lp = logo.getLayoutParams();
        lp.width = size;
        lp.height = size;
        logo.setLayoutParams(lp);
        appTitle.setVisibility(home ? View.VISIBLE : View.GONE);
        getWindow().setSoftInputMode(s == SCREEN_CREATE
                ? WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE
                : WindowManager.LayoutParams.SOFT_INPUT_ADJUST_PAN);
        switch (s) {
            case SCREEN_CREATE:
                inf.inflate(R.layout.screen_create, content, true);
                setupCreate();
                break;
            case SCREEN_STORE:
                inf.inflate(R.layout.screen_store, content, true);
                setupStore();
                break;
            case SCREEN_LINK:
                inf.inflate(R.layout.screen_link, content, true);
                setupLink();
                break;
            case SCREEN_SCOPE:
                inf.inflate(R.layout.screen_scope, content, true);
                setupScope();
                break;
            default:
                inf.inflate(R.layout.screen_home, content, true);
                setupHome();
                break;
        }
        View back = content.findViewById(R.id.btnBack);
        if (back != null) {
            back.setOnClickListener(new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    onBackPressed();
                }
            });
        }
        updateHeader();
    }

    private void setupHome() {
        content.findViewById(R.id.btnCreate).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                openEditor(null, null, 0);
            }
        });
        content.findViewById(R.id.btnStore).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showScreen(SCREEN_STORE);
            }
        });
        content.findViewById(R.id.btnLink).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showScreen(SCREEN_LINK);
            }
        });
    }

    // ---------------- crear ----------------
    private void openEditor(String name, String code, int gotoLine) {
        showScreen(SCREEN_CREATE);
        editingName = name;
        if (name != null) nameEdit.setText(name);
        if (code != null) codeEdit.setText(code);
        updateCursorInfo();
        if (gotoLine > 0) codeEdit.goToLine(gotoLine);
    }

    private void setupCreate() {
        nameEdit = content.findViewById(R.id.progName);
        codeEdit = content.findViewById(R.id.codeEdit);
        cursorInfo = content.findViewById(R.id.cursorInfo);
        editingName = null;
        cursorLine = 1;
        cursorCol = 1;
        codeEdit.setCursorListener(new CodeEditText.CursorListener() {
            @Override
            public void onCursorMoved(int line, int column) {
                cursorLine = line;
                cursorCol = column;
                updateCursorInfo();
            }
        });
        codeEdit.addTextChangedListener(new TextWatcher() {
            @Override
            public void beforeTextChanged(CharSequence s, int start, int count, int after) {
            }

            @Override
            public void onTextChanged(CharSequence s, int start, int before, int count) {
            }

            @Override
            public void afterTextChanged(Editable s) {
                updateCursorInfo();
            }
        });
        content.findViewById(R.id.btnPaste).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                pasteCode();
            }
        });
        content.findViewById(R.id.btnTemplate).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                showExamples();
            }
        });
        content.findViewById(R.id.btnClear).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (codeEdit.length() == 0) return;
                dialog().setTitle("Borrar código")
                        .setMessage("Se borrará todo el código del editor.")
                        .setPositiveButton("BORRAR", new DialogInterface.OnClickListener() {
                            @Override
                            public void onClick(DialogInterface d, int w) {
                                codeEdit.setText("");
                            }
                        })
                        .setNegativeButton("CANCELAR", null)
                        .show();
            }
        });
        content.findViewById(R.id.btnSave).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                saveFromEditor(false);
            }
        });
        content.findViewById(R.id.btnSaveLoad).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                saveFromEditor(true);
            }
        });
        updateCursorInfo();
    }

    private void updateCursorInfo() {
        if (cursorInfo == null || codeEdit == null) return;
        int lines = 1;
        CharSequence t = codeEdit.getText();
        for (int i = 0; i < t.length(); i++) if (t.charAt(i) == '\n') lines++;
        int bytes = t.toString().getBytes(java.nio.charset.StandardCharsets.UTF_8).length;
        cursorInfo.setText(String.format(Locale.getDefault(), "Línea %d, col %d  ·  %d líneas  ·  %s",
                cursorLine, cursorCol, lines, formatSize(bytes)));
    }

    private void pasteCode() {
        ClipboardManager cm = (ClipboardManager) getSystemService(Context.CLIPBOARD_SERVICE);
        ClipData clip = cm != null ? cm.getPrimaryClip() : null;
        if (clip == null || clip.getItemCount() == 0) {
            toast("El portapapeles está vacío");
            return;
        }
        CharSequence text = clip.getItemAt(0).coerceToText(this);
        if (text == null || text.length() == 0) {
            toast("El portapapeles está vacío");
            return;
        }
        Editable e = codeEdit.getText();
        int a = Math.max(0, codeEdit.getSelectionStart());
        int b = Math.max(0, codeEdit.getSelectionEnd());
        if (e.length() == 0) codeEdit.setText(text);
        else e.replace(Math.min(a, b), Math.max(a, b), text);
        toast("Código pegado");
    }

    private void saveFromEditor(final boolean thenLoad) {
        final String name = ProgramStore.cleanName(nameEdit.getText().toString());
        final String code = codeEdit.getText().toString();
        if (name.isEmpty()) {
            nameEdit.setError("Ponle un nombre al programa");
            nameEdit.requestFocus();
            return;
        }
        if (code.trim().isEmpty()) {
            toast("El código está vacío: pega aquí tu programa");
            return;
        }
        if (store.exists(name) && !name.equals(editingName)) {
            dialog().setTitle("Ya existe")
                    .setMessage("Ya hay un programa llamado \"" + name + "\". ¿Quieres reemplazarlo?")
                    .setPositiveButton("REEMPLAZAR", new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface d, int w) {
                            doSave(name, code, thenLoad);
                        }
                    })
                    .setNegativeButton("CANCELAR", null)
                    .show();
            return;
        }
        doSave(name, code, thenLoad);
    }

    private void doSave(String name, String code, boolean thenLoad) {
        try {
            store.save(name, code);
        } catch (IOException e) {
            toast("No se pudo guardar: " + e.getMessage());
            return;
        }
        editingName = name;
        nameEdit.setText(name);
        toast("Guardado \"" + name + "\" en el almacén");
        if (thenLoad) {
            showScreen(SCREEN_STORE);
            loadProgram(name);
        }
    }

    // ---------------- almacen ----------------
    private void setupStore() {
        programList = content.findViewById(R.id.programList);
        emptyView = content.findViewById(R.id.emptyView);
        consoleText = content.findViewById(R.id.consoleText);
        consoleScroll = content.findViewById(R.id.consoleScroll);
        consoleInput = content.findViewById(R.id.consoleInput);
        programAdapter = new ProgramAdapter();
        programList.setAdapter(programAdapter);
        programList.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                loadProgram(programs.get(position).name);
            }
        });
        programList.setOnItemLongClickListener(new AdapterView.OnItemLongClickListener() {
            @Override
            public boolean onItemLongClick(AdapterView<?> parent, View view, int position, long id) {
                programOptions(programs.get(position));
                return true;
            }
        });
        content.findViewById(R.id.btnImport).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                importFile();
            }
        });
        content.findViewById(R.id.btnRun).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (requireLink()) ble.sendRun();
            }
        });
        content.findViewById(R.id.btnStop).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (requireLink()) ble.sendStop();
            }
        });
        content.findViewById(R.id.btnClearConsole).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                ble.clearConsole();
            }
        });
        content.findViewById(R.id.btnSend).setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                sendConsoleInput();
            }
        });
        consoleInput.setOnEditorActionListener(new TextView.OnEditorActionListener() {
            @Override
            public boolean onEditorAction(TextView v, int actionId, KeyEvent event) {
                if (actionId == EditorInfo.IME_ACTION_SEND || actionId == EditorInfo.IME_ACTION_DONE) {
                    sendConsoleInput();
                    return true;
                }
                return false;
            }
        });
        refreshPrograms();
        renderConsole();
    }

    private void sendConsoleInput() {
        String t = consoleInput.getText().toString();
        if (t.isEmpty() || !requireLink()) return;
        ble.sendInput(t);
        consoleInput.setText("");
    }

    private void refreshPrograms() {
        if (programAdapter == null || screen != SCREEN_STORE) return;
        programs.clear();
        programs.addAll(store.list());
        programAdapter.notifyDataSetChanged();
        emptyView.setVisibility(programs.isEmpty() ? View.VISIBLE : View.GONE);
    }

    private void renderConsole() {
        if (consoleText == null || screen != SCREEN_STORE) return;
        CharSequence c = ble.getConsole();
        consoleText.setText(c.length() == 0 ? "La salida de Serial.print de tu programa aparecerá aquí." : c);
        consoleText.setAlpha(c.length() == 0 ? 0.5f : 1f);
        consoleScroll.post(new Runnable() {
            @Override
            public void run() {
                consoleScroll.fullScroll(View.FOCUS_DOWN);
            }
        });
    }

    private boolean requireLink() {
        if (ble.isReady()) return true;
        dialog().setTitle("Sin ESP32 enlazado")
                .setMessage("Primero enlaza un ESP32 que tenga el firmware X-RTbox, por Bluetooth o por USB-C.")
                .setPositiveButton("ENLAZAR", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        showScreen(SCREEN_LINK);
                    }
                })
                .setNegativeButton("CANCELAR", null)
                .show();
        return false;
    }

    private void loadProgram(String name) {
        if (!requireLink()) return;
        String code;
        try {
            code = store.read(name);
        } catch (IOException e) {
            toast("No se pudo leer el programa: " + e.getMessage());
            return;
        }
        ble.sendProgram(name, code);
    }

    private void programOptions(final ProgramStore.Program p) {
        final String[] opts = {"Cargar en el ESP32", "Editar", "Renombrar", "Compartir", "Eliminar"};
        dialog().setTitle(p.name)
                .setItems(opts, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        switch (which) {
                            case 0:
                                loadProgram(p.name);
                                break;
                            case 1:
                                editProgram(p.name, 0);
                                break;
                            case 2:
                                renameProgram(p);
                                break;
                            case 3:
                                shareProgram(p);
                                break;
                            default:
                                deleteProgram(p);
                                break;
                        }
                    }
                })
                .show();
    }

    private void editProgram(String name, int line) {
        try {
            openEditor(name, store.read(name), line);
        } catch (IOException e) {
            toast("No se pudo abrir: " + e.getMessage());
        }
    }

    private void renameProgram(final ProgramStore.Program p) {
        final EditText input = new EditText(this);
        input.setText(p.name);
        input.setSingleLine(true);
        input.setInputType(InputType.TYPE_CLASS_TEXT);
        input.setSelectAllOnFocus(true);
        FrameLayout box = new FrameLayout(this);
        box.setPadding(dp(20), dp(8), dp(20), 0);
        box.addView(input);
        dialog().setTitle("Renombrar")
                .setView(box)
                .setPositiveButton("OK", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        String n = ProgramStore.cleanName(input.getText().toString());
                        if (n.isEmpty() || n.equals(p.name)) return;
                        if (!store.rename(p.name, n)) toast("Ya existe un programa con ese nombre");
                        refreshPrograms();
                    }
                })
                .setNegativeButton("CANCELAR", null)
                .show();
    }

    private void shareProgram(ProgramStore.Program p) {
        try {
            Intent i = new Intent(Intent.ACTION_SEND);
            i.setType("text/plain");
            i.putExtra(Intent.EXTRA_SUBJECT, p.name + ".cpp");
            i.putExtra(Intent.EXTRA_TEXT, store.read(p.name));
            startActivity(Intent.createChooser(i, "Compartir " + p.name));
        } catch (IOException e) {
            toast("No se pudo leer el programa");
        }
    }

    private void deleteProgram(final ProgramStore.Program p) {
        dialog().setTitle("Eliminar")
                .setMessage("¿Eliminar \"" + p.name + "\" del almacén?")
                .setPositiveButton("ELIMINAR", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        store.delete(p.name);
                        refreshPrograms();
                    }
                })
                .setNegativeButton("CANCELAR", null)
                .show();
    }

    @SuppressWarnings("deprecation")
    private void importFile() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        try {
            startActivityForResult(i, REQ_IMPORT);
        } catch (Exception e) {
            toast("No hay ninguna app para elegir archivos");
        }
    }

    private void handleImport(Uri uri) {
        String display = "programa";
        try (Cursor c = getContentResolver().query(uri, new String[]{OpenableColumns.DISPLAY_NAME}, null, null, null)) {
            if (c != null && c.moveToFirst()) {
                String d = c.getString(0);
                if (d != null) display = d;
            }
        } catch (Exception e) {
            // nombre por defecto
        }
        int dot = display.lastIndexOf('.');
        if (dot > 0) display = display.substring(0, dot);
        String code;
        try (InputStream in = getContentResolver().openInputStream(uri)) {
            if (in == null) throw new IOException("no se pudo abrir");
            code = ProgramStore.readAll(in);
        } catch (IOException e) {
            toast("No se pudo importar: " + e.getMessage());
            return;
        }
        String name = store.uniqueName(ProgramStore.cleanName(display));
        try {
            store.save(name, code);
            toast("Importado \"" + name + "\"");
        } catch (IOException e) {
            toast("No se pudo guardar: " + e.getMessage());
        }
        refreshPrograms();
    }

    // ---------------- enlazar ----------------
    private void setupLink() {
        linkState = content.findViewById(R.id.linkState);
        linkDetail = content.findViewById(R.id.linkDetail);
        btnScan = content.findViewById(R.id.btnScan);
        btnUsb = content.findViewById(R.id.btnUsb);
        btnFlash = content.findViewById(R.id.btnFlash);
        btnDisconnect = content.findViewById(R.id.btnDisconnect);
        btnReconnect = content.findViewById(R.id.btnReconnect);
        deviceEmpty = content.findViewById(R.id.deviceEmpty);
        ListView list = content.findViewById(R.id.deviceList);
        deviceAdapter = new DeviceAdapter();
        list.setAdapter(deviceAdapter);
        list.setOnItemClickListener(new AdapterView.OnItemClickListener() {
            @Override
            public void onItemClick(AdapterView<?> parent, View view, int position, long id) {
                final EspLink.Found f = ble.getFound().get(position);
                withBluetooth(new Runnable() {
                    @Override
                    public void run() {
                        ble.connect(f.device, f.name, false);
                    }
                });
            }
        });
        btnScan.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                if (ble.isScanning()) {
                    ble.stopScan();
                    return;
                }
                withBluetooth(new Runnable() {
                    @Override
                    public void run() {
                        if (!ble.startScan()) toast("No se pudo empezar la búsqueda");
                    }
                });
            }
        });
        btnDisconnect.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                ble.disconnect();
            }
        });
        btnUsb.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                withUsbDevice(new UsbAction() {
                    @Override
                    public void run(UsbDevice dev) {
                        ble.connectUsb(dev);
                    }
                });
            }
        });
        btnFlash.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                withUsbDevice(new UsbAction() {
                    @Override
                    public void run(UsbDevice dev) {
                        confirmFlash(dev);
                    }
                });
            }
        });
        btnReconnect.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                withBluetooth(new Runnable() {
                    @Override
                    public void run() {
                        ble.connectAddress(ble.getLastAddress(), ble.getLastName(), false);
                    }
                });
            }
        });
        refreshLink();
    }

    private void refreshLink() {
        if (screen != SCREEN_LINK || linkState == null) return;
        int st = ble.getLinkState();
        boolean usbLink = ble.getTransport() == EspLink.TRANSPORT_USB;
        boolean flashing = ble.isFlashing();
        if (flashing) {
            linkState.setText("⚡ Instalando firmware... " + ble.getFlashPercent() + "%");
            linkState.setTextColor(0xFFE2B3FF);
            linkDetail.setText(ble.getFlashStep() + "\nNo desconectes el cable USB.");
        } else if (st == EspLink.LINK_READY) {
            linkState.setText(usbLink ? "● Enlazado por USB-C (" + ble.getDeviceName() + ")"
                    : "● Enlazado con " + ble.getDeviceName() + " (Bluetooth)");
            linkState.setTextColor(0xFFE2B3FF);
            String fwLine = "";
            if (!ble.getBundledFirmwareVersion().isEmpty() && ble.getRemoteFirmwareVersion() != null) {
                fwLine = ble.isFirmwareOutdated()
                        ? "\n⚠ Firmware del ESP32 antiguo: pulsa ⚡ INSTALAR FIRMWARE"
                        : "\nFirmware del ESP32 al día (" + ble.getRemoteFirmwareVersion() + ")";
            }
            linkDetail.setText(remoteSummary() + (ble.getRemoteHeap() > 0 ? "  ·  " + (ble.getRemoteHeap() / 1024) + " KB libres" : "") + fwLine);
        } else if (st == EspLink.LINK_CONNECTING) {
            linkState.setText(usbLink ? "◌ Conectando por USB-C..." : "◌ Conectando con " + ble.getDeviceName() + "...");
            linkState.setTextColor(0xFFFFFFFF);
            linkDetail.setText("Espera un momento");
        } else {
            linkState.setText("○ Sin conexión");
            linkState.setTextColor(0xFFFFFFFF);
            linkDetail.setText("Conéctalo por Bluetooth o con un cable al USB-C del móvil");
        }
        btnScan.setText(ble.isScanning() ? "PARAR" : "BLUETOOTH");
        setUsable(btnScan, !flashing);
        setUsable(btnUsb, !flashing);
        setUsable(btnFlash, !flashing);
        btnDisconnect.setVisibility(st == EspLink.LINK_IDLE || flashing ? View.GONE : View.VISIBLE);
        String last = ble.getLastAddress();
        boolean showReconnect = last != null && st == EspLink.LINK_IDLE && !flashing;
        btnReconnect.setVisibility(showReconnect ? View.VISIBLE : View.GONE);
        if (showReconnect) btnReconnect.setText("Reconectar con " + ble.getLastName() + " (Bluetooth)");
        deviceAdapter.notifyDataSetChanged();
        deviceEmpty.setVisibility(ble.getFound().isEmpty() ? View.VISIBLE : View.GONE);
        deviceEmpty.setText(ble.isScanning()
                ? "Buscando... acerca el ESP32 al móvil"
                : "Pulsa BLUETOOTH para buscar, o USB-C si lo tienes conectado por cable. El ESP32 necesita el firmware X-RTbox (si no lo tiene, conéctalo por cable y pulsa INSTALAR FIRMWARE).");
    }

    private static void setUsable(View v, boolean on) {
        v.setEnabled(on);
        v.setAlpha(on ? 1f : 0.4f);
    }

    // ---------------- Osciloscopio ----------------
    private void setupScope() {
        scopeList = content.findViewById(R.id.scopeList);
        scopeStatus = content.findViewById(R.id.scopeStatus);
        scopeTime = content.findViewById(R.id.scopeTime);
        scopeVolt = content.findViewById(R.id.scopeVolt);
        scopeTrig = content.findViewById(R.id.scopeTrig);
        btnScopeRun = content.findViewById(R.id.btnScopeRun);
        DPadView pad = content.findViewById(R.id.dpad);
        pad.setListener(new DPadView.Listener() {
            @Override
            public void onPad(int dir) {
                switch (dir) {
                    case DPadView.LEFT:
                        if (scopeTimeIdx > 0) scopeTimeIdx--;
                        break;
                    case DPadView.RIGHT:
                        if (scopeTimeIdx < TIME_DIVS_US.length - 1) scopeTimeIdx++;
                        break;
                    case DPadView.UP:
                        if (scopeVoltIdx < VOLT_DIVS.length - 1) scopeVoltIdx++;
                        break;
                    default:
                        if (scopeVoltIdx > 0) scopeVoltIdx--;
                        break;
                }
                if (dir == DPadView.LEFT || dir == DPadView.RIGHT) ble.setScopeTimebase(TIME_DIVS_US[scopeTimeIdx]);
                for (int k = 0; k < scopeList.getChildCount(); k++) {
                    View v = scopeList.getChildAt(k);
                    if (v instanceof ScopeView) ((ScopeView) v).setVoltsPerDiv(VOLT_DIVS[scopeVoltIdx]);
                }
                updateScopeLabels();
            }
        });
        btnScopeRun.setOnClickListener(new View.OnClickListener() {
            @Override
            public void onClick(View v) {
                scopePaused = !scopePaused;
                if (scopePaused) ble.stopScope();
                else startScope();
                updateScopeLabels();
            }
        });
        updateScopeLabels();
        updateScopeStatus(null);
        if (!scopePaused) startScope();
    }

    private void startScope() {
        ble.startScope(TIME_DIVS_US[scopeTimeIdx], scopeTrigPin);
    }

    private static String timeDivText(int us) {
        if (us >= 1000000) return (us / 1000000) + " s/div";
        if (us >= 1000) return (us / 1000) + " ms/div";
        return us + " µs/div";
    }

    private void updateScopeLabels() {
        if (screen != SCREEN_SCOPE || scopeTime == null) return;
        scopeTime.setText("⏱ " + timeDivText(TIME_DIVS_US[scopeTimeIdx]));
        double v = VOLT_DIVS[scopeVoltIdx];
        scopeVolt.setText("⚡ " + (v < 1 ? String.format(Locale.US, "%d mV/div", Math.round(v * 1000)) : String.format(Locale.US, "%.0f V/div", v)));
        scopeTrig.setText("Disparo: " + (scopeTrigPin >= 0 ? "GPIO " + scopeTrigPin : "primer pin") + " ↑");
        btnScopeRun.setText(scopePaused ? R.string.scope_resume : R.string.scope_pause);
    }

    private void updateScopeStatus(EspLink.ScopeFrame f) {
        if (screen != SCREEN_SCOPE || scopeStatus == null) return;
        if (ble.getLinkState() != EspLink.LINK_READY) {
            scopeStatus.setText("Enlaza el ESP32 (ENLAZAR ESP32) para ver las señales de sus pines.");
        } else if (scopePaused) {
            scopeStatus.setText("En pausa: pulsa SEGUIR para volver a medir.");
        } else if (f == null) {
            scopeStatus.setText("Midiendo los pines de salida del ESP32...");
        } else if (f.pins.length == 0) {
            scopeStatus.setText("El ESP32 no tiene ningún pin de salida activo: carga un programa que use pines como salida.");
        } else {
            StringBuilder b = new StringBuilder("Pines activos: ");
            for (int k = 0; k < f.pins.length; k++) b.append(k > 0 ? ", " : "").append(f.pins[k]);
            b.append(f.triggered ? "  ·  disparado" : "  ·  sin disparo (libre)");
            if (ble.getRemoteFirmwareVersion() != null && ble.isFirmwareOutdated()) b.append("\n⚠ Actualiza el firmware del ESP32");
            scopeStatus.setText(b);
        }
    }

    @Override
    public void onScopeData(EspLink.ScopeFrame f) {
        if (screen != SCREEN_SCOPE || scopeList == null) return;
        // una pantalla por pin activo (se rehace si cambian los pines)
        boolean same = scopeList.getChildCount() == f.pins.length;
        for (int k = 0; same && k < f.pins.length; k++) same = ((ScopeView) scopeList.getChildAt(k)).getPin() == f.pins[k];
        if (!same) {
            scopeList.removeAllViews();
            for (int k = 0; k < f.pins.length; k++) {
                final ScopeView sv = new ScopeView(this);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
                if (k > 0) lp.topMargin = dp(10);
                sv.setLayoutParams(lp);
                sv.setVoltsPerDiv(VOLT_DIVS[scopeVoltIdx]);
                sv.setOnClickListener(new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        scopeTrigPin = sv.getPin();
                        ble.setScopeTrigger(scopeTrigPin);
                        updateScopeLabels();
                        toast("Disparo con el GPIO " + scopeTrigPin);
                    }
                });
                scopeList.addView(sv);
            }
        }
        for (int k = 0; k < f.pins.length; k++) {
            ((ScopeView) scopeList.getChildAt(k)).setTrace(f.pins[k], f.data[k], f.n, f.analog[k], f.windowUs,
                    f.pins[k] == f.trigPin, f.triggered);
        }
        updateScopeStatus(f);
    }

    // ---------------- USB ----------------
    private interface UsbAction {
        void run(UsbDevice d);
    }

    /** Busca el ESP32 conectado por USB, pide permiso y ejecuta la accion. */
    private void withUsbDevice(final UsbAction action) {
        final List<UsbDevice> list = ble.getUsbDevices();
        if (list.isEmpty()) {
            dialog().setTitle("No hay ningún ESP32 por USB")
                    .setMessage("Conecta el ESP32 al USB-C del móvil con un cable (USB-C a USB-C, o un adaptador OTG si tu placa es micro-USB) y vuelve a pulsar.\n\n"
                            + "Si no aparece, prueba otro cable: algunos solo cargan y no transmiten datos.")
                    .setPositiveButton("OK", null)
                    .show();
            return;
        }
        if (list.size() == 1) {
            askUsb(list.get(0), action);
            return;
        }
        String[] names = new String[list.size()];
        for (int k = 0; k < names.length; k++) names[k] = UsbSerial.describe(list.get(k)) + "  (" + list.get(k).getDeviceName() + ")";
        dialog().setTitle("Elige el dispositivo USB")
                .setItems(names, new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        askUsb(list.get(which), action);
                    }
                })
                .show();
    }

    private void askUsb(final UsbDevice d, final UsbAction action) {
        ble.requestUsbPermission(d, new EspLink.UsbPermissionCallback() {
            @Override
            public void onResult(boolean granted) {
                if (granted) action.run(d);
                else toast("Sin permiso no se puede usar el USB");
            }
        });
    }

    private static String chipLabel(String asset) {
        switch (asset) {
            case "esp32":
                return "ESP32";
            case "esp32s3":
                return "ESP32-S3";
            case "esp32c3":
                return "ESP32-C3";
            case "esp32c6":
                return "ESP32-C6";
            default:
                return asset;
        }
    }

    private void confirmFlash(final UsbDevice d) {
        List<String> fw = ble.bundledFirmwares();
        if (fw.isEmpty()) {
            dialog().setTitle("Falta el firmware")
                    .setMessage("Esta versión de la app no trae el firmware dentro. Instala el APK que compila GitHub (incluye el firmware para ESP32, S3, C3 y C6).")
                    .setPositiveButton("OK", null)
                    .show();
            return;
        }
        StringBuilder chips = new StringBuilder();
        for (String a : fw) {
            if (chips.length() > 0) chips.append(", ");
            chips.append(chipLabel(a));
        }
        dialog().setTitle("Instalar firmware X-RTbox")
                .setMessage("Se grabará en el ESP32 conectado por USB (" + UsbSerial.describe(d) + ") el firmware X-RTbox, que permite usarlo por Bluetooth y por USB-C.\n\n"
                        + "Se borrará el programa que tenga ahora el ESP32. Tarda entre medio minuto y dos minutos: no desconectes el cable.\n\n"
                        + "Chips compatibles: " + chips + ".")
                .setPositiveButton("INSTALAR", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface dlg, int w) {
                        ble.flashFirmware(d);
                    }
                })
                .setNegativeButton("CANCELAR", null)
                .show();
    }

    // ------------------------------------------------------------------
    //  Cabecera
    // ------------------------------------------------------------------
    private String remoteSummary() {
        String p = ble.getRemoteProgram();
        String q = p.isEmpty() ? "" : " \"" + p + "\"";
        switch (ble.getRemoteState()) {
            case "RUN":
                return "Ejecutando" + q;
            case "STOP":
                return "Detenido" + q;
            case "ERR":
                return "Error en" + q;
            case "FIN":
                return "Terminado" + q;
            default:
                return "Sin programa";
        }
    }

    private void updateHeader() {
        int st = ble.getLinkState();
        boolean usbLink = ble.getTransport() == EspLink.TRANSPORT_USB;
        if (ble.isFlashing()) {
            statusText.setText("⚡ Instalando firmware por USB-C");
            statusText.setTextColor(0xFFE2B3FF);
        } else if (st == EspLink.LINK_READY) {
            statusText.setText("● " + (usbLink ? "USB-C" : ble.getDeviceName()) + "  ·  " + remoteSummary());
            statusText.setTextColor(0xFFE2B3FF);
        } else if (st == EspLink.LINK_CONNECTING) {
            statusText.setText(usbLink ? "◌ Conectando por USB-C" : "◌ Conectando con " + ble.getDeviceName());
            statusText.setTextColor(0xFFFFFFFF);
        } else {
            statusText.setText("○ Sin conexión");
            statusText.setTextColor(0xFF9C93AD);
        }
    }

    // ------------------------------------------------------------------
    //  EspLink.Listener
    // ------------------------------------------------------------------
    @Override
    public void onLinkChanged() {
        updateHeader();
        refreshLink();
        if (screen == SCREEN_SCOPE && ble.getLinkState() != EspLink.LINK_READY) updateScopeStatus(null);
        if (screen == SCREEN_STORE && programAdapter != null) programAdapter.notifyDataSetChanged();
    }

    @Override
    public void onBusyChanged() {
        String b = ble.getBusyText();
        logo.setBusy(b != null);
        busyLabel.setText(b == null ? "" : b + "...");
        busyLabel.setVisibility(b == null ? View.INVISIBLE : View.VISIBLE);
        if (ble.isFlashing()) getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        updateHeader();
        refreshLink();
    }

    @Override
    public void onConsoleChanged() {
        renderConsole();
    }

    @Override
    public void onDevicesChanged() {
        refreshLink();
    }

    @Override
    public void onProgramResult(boolean ok, final String programName, String message) {
        if (isFinishing() || isDestroyed()) return;
        if (ok) {
            toast("\"" + programName + "\" cargado en el ESP32");
            if (programAdapter != null) programAdapter.notifyDataSetChanged();
            return;
        }
        final int line = errorLine(message);
        final boolean oldFw = ble.isFirmwareOutdated();
        if (oldFw) message += "\n\n⚠ El ESP32 tiene un firmware X-RTbox antiguo: muchos errores como este desaparecen al actualizarlo.";
        AlertDialog.Builder b = dialog()
                .setTitle(programName == null || programName.isEmpty() ? "Error" : "Error en \"" + programName + "\"")
                .setMessage(message)
                .setPositiveButton("OK", null);
        if (oldFw) {
            b.setNegativeButton("ACTUALIZAR FIRMWARE", new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface d, int w) {
                    offerFirmwareUpdate(ble.getRemoteFirmwareVersion());
                }
            });
        }
        if (line > 0 && programName != null && store.exists(programName)) {
            b.setNeutralButton("IR A LA LÍNEA " + line, new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface d, int w) {
                    editProgram(programName, line);
                }
            });
        }
        b.show();
    }

    @Override
    public void onFirmwareOutdated(String remoteVersion) {
        if (isFinishing() || isDestroyed() || ble.isFlashing()) return;
        offerFirmwareUpdate(remoteVersion);
    }

    private void offerFirmwareUpdate(String remote) {
        String bundled = ble.getBundledFirmwareVersion();
        String what;
        if (remote == null || remote.isEmpty()) what = "un firmware X-RTbox antiguo";
        else if ("dev".equals(remote)) what = "un firmware X-RTbox compilado a mano";
        else what = "otra versión del firmware X-RTbox (" + remote + ")";
        String msg = "El ESP32 tiene " + what + " y esta app trae la versión " + bundled + ".\n\n"
                + "Con un firmware antiguo, los programas que usan funciones nuevas (ledc_..., struct, signalSquare, Servo, Wire...) "
                + "dan error de compilación aunque estén bien escritos.";
        final UsbDevice dev = ble.getConnectedUsbDevice();
        AlertDialog.Builder b = dialog().setTitle("⚠ Actualiza el firmware del ESP32");
        if (dev != null) {
            b.setMessage(msg + "\n\nPulsa INSTALAR AHORA: tarda menos de dos minutos y no hay que desconectar nada. "
                    + "Se borra el programa que tenga cargado el ESP32; los de tu almacén no se tocan.")
                    .setPositiveButton("INSTALAR AHORA", new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface d, int w) {
                            ble.flashFirmware(dev);
                        }
                    })
                    .setNegativeButton("MÁS TARDE", null);
        } else {
            b.setMessage(msg + "\n\nConecta el ESP32 al móvil con un cable USB-C y pulsa ENLAZAR ESP32 → ⚡ INSTALAR FIRMWARE.")
                    .setPositiveButton("OK", null);
        }
        b.show();
    }

    @Override
    public void onLinkMessage(String message) {
        if (!isFinishing()) toast(message);
    }

    @Override
    public void onFlashFinished(boolean ok, String message) {
        if (isFinishing() || isDestroyed()) return;
        AlertDialog.Builder b = dialog()
                .setTitle(ok ? "Firmware instalado" : "No se pudo instalar")
                .setMessage(message);
        if (ok) {
            b.setPositiveButton("ENLAZAR POR USB-C", new DialogInterface.OnClickListener() {
                @Override
                public void onClick(DialogInterface d, int w) {
                    withUsbDevice(new UsbAction() {
                        @Override
                        public void run(UsbDevice dev) {
                            ble.connectUsb(dev);
                        }
                    });
                }
            });
            b.setNegativeButton("CERRAR", null);
        } else {
            b.setPositiveButton("OK", null);
        }
        b.show();
    }

    private static int errorLine(String msg) {
        if (msg == null) return 0;
        Matcher m = Pattern.compile("[Ll][ií]nea (\\d+)").matcher(msg);
        if (m.find()) {
            try {
                return Integer.parseInt(m.group(1));
            } catch (NumberFormatException e) {
                return 0;
            }
        }
        return 0;
    }

    // ------------------------------------------------------------------
    //  Permisos y Bluetooth
    // ------------------------------------------------------------------
    private String[] neededPermissions() {
        List<String> need = new ArrayList<>();
        if (Build.VERSION.SDK_INT >= 31) {
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_SCAN) != PackageManager.PERMISSION_GRANTED)
                need.add(Manifest.permission.BLUETOOTH_SCAN);
            if (checkSelfPermission(Manifest.permission.BLUETOOTH_CONNECT) != PackageManager.PERMISSION_GRANTED)
                need.add(Manifest.permission.BLUETOOTH_CONNECT);
        } else {
            if (checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) != PackageManager.PERMISSION_GRANTED)
                need.add(Manifest.permission.ACCESS_FINE_LOCATION);
        }
        return need.toArray(new String[0]);
    }

    @SuppressWarnings("deprecation")
    private boolean locationEnabled() {
        if (Build.VERSION.SDK_INT >= 31) return true;  // no hace falta en Android 12+
        LocationManager lm = (LocationManager) getSystemService(Context.LOCATION_SERVICE);
        if (Build.VERSION.SDK_INT >= 28) return lm != null && lm.isLocationEnabled();
        try {
            return Settings.Secure.getInt(getContentResolver(), Settings.Secure.LOCATION_MODE) != Settings.Secure.LOCATION_MODE_OFF;
        } catch (Settings.SettingNotFoundException e) {
            return true;
        }
    }

    /** Ejecuta la accion cuando hay permisos, Bluetooth encendido y (en Android antiguo) ubicacion. */
    @SuppressWarnings("deprecation")
    private void withBluetooth(Runnable action) {
        if (!ble.isSupported()) {
            toast("Este móvil no tiene Bluetooth LE");
            return;
        }
        String[] need = neededPermissions();
        if (need.length > 0) {
            afterPermission = action;
            requestPermissions(need, REQ_PERMS);
            return;
        }
        if (!ble.isEnabled()) {
            afterPermission = action;
            try {
                startActivityForResult(new Intent(BluetoothAdapter.ACTION_REQUEST_ENABLE), REQ_ENABLE_BT);
            } catch (SecurityException e) {
                toast("Enciende el Bluetooth desde los ajustes");
            }
            return;
        }
        if (!locationEnabled()) {
            dialog().setTitle("Activa la ubicación")
                    .setMessage("En esta versión de Android hay que tener la ubicación activada para buscar dispositivos Bluetooth. X-RTbox no usa tu ubicación.")
                    .setPositiveButton("AJUSTES", new DialogInterface.OnClickListener() {
                        @Override
                        public void onClick(DialogInterface d, int w) {
                            startActivity(new Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS));
                        }
                    })
                    .setNegativeButton("CANCELAR", null)
                    .show();
            return;
        }
        action.run();
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode != REQ_PERMS) return;
        boolean all = grantResults.length > 0;
        for (int g : grantResults) if (g != PackageManager.PERMISSION_GRANTED) all = false;
        Runnable a = afterPermission;
        afterPermission = null;
        if (all && a != null) withBluetooth(a);
        else if (!all) toast("Sin el permiso de Bluetooth no se puede enlazar el ESP32");
    }

    @SuppressWarnings("deprecation")
    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQ_ENABLE_BT) {
            Runnable a = afterPermission;
            afterPermission = null;
            if (resultCode == RESULT_OK && a != null) withBluetooth(a);
        } else if (requestCode == REQ_IMPORT && resultCode == RESULT_OK && data != null && data.getData() != null) {
            handleImport(data.getData());
        }
    }

    private void autoReconnect() {
        if (triedAutoConnect) return;
        triedAutoConnect = true;
        if (ble.getLinkState() != EspLink.LINK_IDLE) return;
        for (UsbDevice d : ble.getUsbDevices()) {
            if (ble.hasUsbPermission(d)) {
                ble.connectUsb(d);
                return;
            }
        }
        String last = ble.getLastAddress();
        if (last == null) return;
        if (neededPermissions().length > 0 || !ble.isEnabled()) return;
        ble.connectAddress(last, ble.getLastName(), true);
    }

    // ------------------------------------------------------------------
    //  Utilidades
    // ------------------------------------------------------------------
    // ---- ejemplos incluidos en la app (assets/ejemplos/*.cpp) ----
    private void showExamples() {
        final List<String> files = new ArrayList<>();
        final List<String> titles = new ArrayList<>();
        try {
            String[] list = getAssets().list("ejemplos");
            if (list != null) {
                java.util.Arrays.sort(list);
                for (String f : list) {
                    if (!f.endsWith(".cpp")) continue;
                    String code = readAsset("ejemplos/" + f);
                    if (code == null) continue;
                    String name = f.substring(0, f.length() - 4);
                    String first = code.split("\n", 2)[0].trim();
                    String title = first.startsWith("//") ? first.substring(2).trim() : name;
                    files.add(f);
                    titles.add(name.replace('_', ' ') + "\n" + title);
                }
            }
        } catch (IOException e) {
            // sin ejemplos: se usa la plantilla
        }
        if (files.isEmpty()) {
            applyExample("Parpadeo", TEMPLATE);
            return;
        }
        dialog().setTitle("Ejemplos")
                .setItems(titles.toArray(new CharSequence[0]), new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int which) {
                        String f = files.get(which);
                        String code = readAsset("ejemplos/" + f);
                        if (code != null) applyExample(f.substring(0, f.length() - 4).replace('_', ' '), code);
                    }
                })
                .setNegativeButton("CANCELAR", null)
                .show();
    }

    private void applyExample(final String name, final String code) {
        if (codeEdit.length() == 0) {
            codeEdit.setText(code);
            nameEdit.setText(name);
            return;
        }
        dialog().setTitle("Ejemplo: " + name)
                .setMessage("Se reemplazará el código actual por este ejemplo. ¿Continuar?")
                .setPositiveButton("REEMPLAZAR", new DialogInterface.OnClickListener() {
                    @Override
                    public void onClick(DialogInterface d, int w) {
                        codeEdit.setText(code);
                        nameEdit.setText(name);
                    }
                })
                .setNegativeButton("CANCELAR", null)
                .show();
    }

    private String readAsset(String path) {
        try (InputStream in = getAssets().open(path)) {
            java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
            byte[] b = new byte[4096];
            int n;
            while ((n = in.read(b)) > 0) out.write(b, 0, n);
            return new String(out.toByteArray(), java.nio.charset.StandardCharsets.UTF_8);
        } catch (IOException e) {
            return null;
        }
    }

    private AlertDialog.Builder dialog() {
        return new AlertDialog.Builder(this, R.style.DialogTheme);
    }

    private void toast(String m) {
        Toast.makeText(this, m, Toast.LENGTH_SHORT).show();
    }

    private static String formatSize(long bytes) {
        if (bytes < 1024) return bytes + " B";
        return String.format(Locale.getDefault(), "%.1f KB", bytes / 1024.0);
    }

    // ------------------------------------------------------------------
    //  Adaptadores
    // ------------------------------------------------------------------
    private class ProgramAdapter extends BaseAdapter {
        private final DateFormat df = DateFormat.getDateTimeInstance(DateFormat.SHORT, DateFormat.SHORT);

        @Override
        public int getCount() {
            return programs.size();
        }

        @Override
        public Object getItem(int position) {
            return programs.get(position);
        }

        @Override
        public long getItemId(int position) {
            return position;
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            View v = convertView != null ? convertView : getLayoutInflater().inflate(R.layout.item_program, parent, false);
            ProgramStore.Program p = programs.get(position);
            ((TextView) v.findViewById(R.id.itemName)).setText(p.name);
            ((TextView) v.findViewById(R.id.itemInfo)).setText(formatSize(p.size) + "  ·  " + df.format(new Date(p.modified)));
            TextView badge = v.findViewById(R.id.itemBadge);
            String badgeText = null;
            if (ble.isReady() && EspLink.wireName(p.name).equals(ble.getRemoteProgram())) {
                switch (ble.getRemoteState()) {
                    case "RUN":
                        badgeText = "EN EJECUCIÓN";
                        break;
                    case "ERR":
                        badgeText = "ERROR";
                        break;
                    case "STOP":
                        badgeText = "DETENIDO";
                        break;
                    case "FIN":
                        badgeText = "TERMINADO";
                        break;
                    default:
                        break;
                }
            }
            badge.setVisibility(badgeText == null ? View.GONE : View.VISIBLE);
            if (badgeText != null) badge.setText(badgeText);
            return v;
        }
    }

    private class DeviceAdapter extends BaseAdapter {
        @Override
        public int getCount() {
            return ble.getFound().size();
        }

        @Override
        public Object getItem(int position) {
            return ble.getFound().get(position);
        }

        @Override
        public long getItemId(int position) {
            return position;
        }

        @Override
        public View getView(int position, View convertView, ViewGroup parent) {
            View v = convertView != null ? convertView : getLayoutInflater().inflate(R.layout.item_device, parent, false);
            EspLink.Found f = ble.getFound().get(position);
            ((TextView) v.findViewById(R.id.devName)).setText(f.name);
            String bars = f.rssi > -60 ? "▮▮▮▮" : f.rssi > -70 ? "▮▮▮▯" : f.rssi > -80 ? "▮▮▯▯" : "▮▯▯▯";
            ((TextView) v.findViewById(R.id.devInfo)).setText(f.device.getAddress() + "   " + bars + " " + f.rssi + " dBm");
            return v;
        }
    }
}
