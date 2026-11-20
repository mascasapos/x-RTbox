package com.xrtbox.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.Typeface;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.View;

import java.util.Locale;

/**
 * Pantalla de osciloscopio de un pin: fondo negro, rejilla de 10 x 8 divisiones
 * y la senal en verde fosforito. 0 V esta una division por encima del borde de abajo.
 */
public class ScopeView extends View {

    public static final int PHOSPHOR = 0xFF39FF14;
    private static final int GRID = 0xFF0E3A0E;
    private static final int GRID_AXIS = 0xFF1F7A1F;
    private static final int TEXT = 0xFF9DFF8A;
    private static final int DIVS_X = 10;
    private static final int DIVS_Y = 8;
    private static final double VREF = 3.3;

    private final Paint grid = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint axis = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint border = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint glow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint trace = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint small = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();

    private byte[] data;
    private int n;
    private boolean analog;
    private int pin = -1;
    private double windowUs = 10000;
    private double voltsPerDiv = 1.0;
    private boolean isTrigger;
    private boolean triggered;
    private String meas = "";

    public ScopeView(Context c) {
        super(c);
        init();
    }

    public ScopeView(Context c, AttributeSet a) {
        super(c, a);
        init();
    }

    private float dp(float v) {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private void init() {
        grid.setColor(GRID);
        grid.setStrokeWidth(dp(1));
        axis.setColor(GRID_AXIS);
        axis.setStrokeWidth(dp(1));
        border.setColor(GRID_AXIS);
        border.setStyle(Paint.Style.STROKE);
        border.setStrokeWidth(dp(1.5f));
        glow.setColor(PHOSPHOR);
        glow.setAlpha(70);
        glow.setStyle(Paint.Style.STROKE);
        glow.setStrokeWidth(dp(6));
        glow.setStrokeJoin(Paint.Join.ROUND);
        glow.setStrokeCap(Paint.Cap.ROUND);
        trace.setColor(PHOSPHOR);
        trace.setStyle(Paint.Style.STROKE);
        trace.setStrokeWidth(dp(2));
        trace.setStrokeJoin(Paint.Join.ROUND);
        trace.setStrokeCap(Paint.Cap.ROUND);
        text.setColor(TEXT);
        text.setTextSize(dp(13));
        text.setTypeface(Typeface.create(Typeface.MONOSPACE, Typeface.BOLD));
        small.setColor(TEXT);
        small.setTextSize(dp(10));
        small.setTypeface(Typeface.MONOSPACE);
    }

    public int getPin() {
        return pin;
    }

    public void setVoltsPerDiv(double v) {
        voltsPerDiv = v;
        invalidate();
    }

    public void setTrace(int pin, byte[] d, int n, boolean analog, double windowUs, boolean isTrigger, boolean triggered) {
        this.pin = pin;
        this.data = d;
        this.n = n;
        this.analog = analog;
        this.windowUs = windowUs;
        this.isTrigger = isTrigger;
        this.triggered = triggered;
        this.meas = measure();
        invalidate();
    }

    /** Frecuencia, ciclo de trabajo y tension de pico a pico. */
    private String measure() {
        if (data == null || n < 2) return "";
        int mn = 255, mx = 0, above = 0, rises = 0, first = -1, last = -1;
        long sum = 0;
        boolean prev = (data[0] & 0xFF) >= 128;
        for (int i = 0; i < n; i++) {
            int v = data[i] & 0xFF;
            sum += v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
            boolean hi = v >= 128;
            if (hi) above++;
            if (hi && !prev) {
                rises++;
                if (first < 0) first = i;
                last = i;
            }
            prev = hi;
        }
        StringBuilder b = new StringBuilder();
        if (rises >= 2 && last > first) {
            double dt = windowUs / n * 1e-6;
            double f = (rises - 1) / ((last - first) * dt);
            b.append(freqText(f)).append("  ");
        }
        double vpp = (mx - mn) / 255.0 * VREF;
        if (analog) {
            b.append(String.format(Locale.US, "Vpp %.2f V  med %.2f V", vpp, sum / (double) n / 255.0 * VREF));
        } else if (vpp < 0.3) {
            b.append(String.format(Locale.US, "%.2f V", mx / 255.0 * VREF));
        } else {
            b.append(String.format(Locale.US, "D %d%%  Vpp %.1f V", Math.round(above * 100.0 / n), vpp));
        }
        return b.toString();
    }

    private static String freqText(double f) {
        if (f >= 1e6) return String.format(Locale.US, "%.3f MHz", f / 1e6);
        if (f >= 1e3) return String.format(Locale.US, "%.3f kHz", f / 1e3);
        if (f >= 10) return String.format(Locale.US, "%.1f Hz", f);
        return String.format(Locale.US, "%.2f Hz", f);
    }

    @Override
    protected void onMeasure(int wSpec, int hSpec) {
        int w = MeasureSpec.getSize(wSpec);
        setMeasuredDimension(w, w * DIVS_Y / DIVS_X);
    }

    @Override
    protected void onDraw(Canvas c) {
        float w = getWidth(), h = getHeight();
        float dx = w / DIVS_X, dy = h / DIVS_Y;
        c.drawColor(0xFF000000);
        // rejilla: 10 divisiones de tiempo y 8 de tension
        for (int i = 1; i < DIVS_X; i++) c.drawLine(i * dx, 0, i * dx, h, i == DIVS_X / 2 ? axis : grid);
        for (int j = 1; j < DIVS_Y; j++) c.drawLine(0, j * dy, w, j * dy, j == DIVS_Y / 2 ? axis : grid);
        float tick = dp(3);
        for (int i = 0; i < DIVS_X * 5; i++) {   // subdivisiones en los ejes centrales
            float x = i * dx / 5;
            c.drawLine(x, h / 2 - tick, x, h / 2 + tick, axis);
        }
        for (int j = 0; j < DIVS_Y * 5; j++) {
            float y = j * dy / 5;
            c.drawLine(w / 2 - tick, y, w / 2 + tick, y, axis);
        }
        c.drawRect(1, 1, w - 1, h - 1, border);
        float y0 = h - dy;   // 0 V
        c.drawText("0V", dp(3), y0 - dp(3), small);
        c.drawLine(0, y0, dp(10), y0, trace);
        // senal
        if (data != null && n >= 2) {
            path.reset();
            float scale = (float) (dy / voltsPerDiv * VREF / 255.0);
            for (int i = 0; i < n; i++) {
                float x = i * w / (n - 1);
                float y = y0 - (data[i] & 0xFF) * scale;
                if (y < 0) y = 0;
                if (y > h) y = h;
                if (i == 0) path.moveTo(x, y);
                else path.lineTo(x, y);
            }
            c.drawPath(path, glow);
            c.drawPath(path, trace);
        }
        // textos
        String name = pin >= 0 ? "GPIO " + pin + (analog ? " ~" : "") : "";
        if (isTrigger) name += triggered ? "  T↑" : "  T?";
        c.drawText(name, dp(6), dp(16), text);
        float mw = small.measureText(meas);
        c.drawText(meas, w - mw - dp(6), dp(15), small);
    }
}
