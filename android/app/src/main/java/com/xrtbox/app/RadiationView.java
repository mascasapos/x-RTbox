package com.xrtbox.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.util.AttributeSet;
import android.view.View;

/**
 * Logo de radiactividad animado (diseno propio).
 * Esta quieto cuando no pasa nada y gira, con un anillo de carga y un brillo
 * pulsante, mientras la app esta haciendo algo (buscar, conectar, cargar...).
 */
public class RadiationView extends View {

    private static final int NEON = 0xFFB026FF;
    private static final int NEON_LIGHT = 0xFFE2B3FF;
    private static final int NEON_DARK = 0xFF5A0F99;

    private final Paint bladePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bladeEdge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint corePaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint haloPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ringPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint arcPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path blades = new Path();
    private final RectF arcRect = new RectF();

    private boolean busy = false;
    private float angle = 0f;       // giro del trebol
    private float speed = 0f;       // grados por segundo
    private float phase = 0f;       // pulso del brillo
    private long lastFrame = 0L;
    private float cx, cy, radius, coreR;

    public RadiationView(Context context) {
        this(context, null);
    }

    public RadiationView(Context context, AttributeSet attrs) {
        super(context, attrs);
        bladePaint.setStyle(Paint.Style.FILL);
        bladeEdge.setStyle(Paint.Style.STROKE);
        bladeEdge.setColor(NEON_LIGHT);
        corePaint.setStyle(Paint.Style.FILL);
        ringPaint.setStyle(Paint.Style.STROKE);
        ringPaint.setColor(NEON);
        arcPaint.setStyle(Paint.Style.STROKE);
        arcPaint.setColor(Color.WHITE);
        arcPaint.setStrokeCap(Paint.Cap.ROUND);
    }

    /** Activa o desactiva el giro (modo "cargando"). */
    public void setBusy(boolean b) {
        if (busy == b) return;
        busy = b;
        lastFrame = 0L;
        postInvalidateOnAnimation();
    }

    public boolean isBusy() {
        return busy;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        cx = w / 2f;
        cy = h / 2f;
        radius = Math.min(w, h) / 2f * 0.98f;
        if (radius <= 0) return;

        // Proporciones del simbolo clasico: nucleo r0, aspas de 1.5*r0 a 5*r0
        float outer = radius * 0.66f;
        coreR = outer / 5f;
        float inner = coreR * 1.5f;
        blades.reset();
        RectF o = new RectF(cx - outer, cy - outer, cx + outer, cy + outer);
        RectF i = new RectF(cx - inner, cy - inner, cx + inner, cy + inner);
        for (int k = 0; k < 3; k++) {
            float start = -120f + k * 120f;   // centro del aspa en -90, 30, 150 grados
            blades.arcTo(o, start, 60f, true);
            blades.arcTo(i, start + 60f, -60f, false);
            blades.close();
        }
        bladePaint.setShader(new RadialGradient(cx, cy, outer,
                new int[]{NEON_LIGHT, NEON, NEON_DARK}, new float[]{0.15f, 0.55f, 1f}, Shader.TileMode.CLAMP));
        corePaint.setShader(new RadialGradient(cx, cy, coreR,
                new int[]{Color.WHITE, NEON_LIGHT, NEON}, new float[]{0f, 0.45f, 1f}, Shader.TileMode.CLAMP));
        haloPaint.setShader(new RadialGradient(cx, cy, radius,
                new int[]{0xC0B026FF, 0x60B026FF, 0x00000000}, new float[]{0f, 0.6f, 1f}, Shader.TileMode.CLAMP));
        bladeEdge.setStrokeWidth(Math.max(1f, radius * 0.012f));
        ringPaint.setStrokeWidth(Math.max(1f, radius * 0.018f));
        arcPaint.setStrokeWidth(Math.max(2f, radius * 0.035f));
        float ar = radius * 0.95f;
        arcRect.set(cx - ar, cy - ar, cx + ar, cy + ar);
    }

    @Override
    protected void onDraw(Canvas c) {
        super.onDraw(c);
        if (radius <= 0) return;
        long now = System.nanoTime();
        float dt = lastFrame == 0L ? 0f : Math.min(0.05f, (now - lastFrame) / 1e9f);
        lastFrame = now;

        // velocidad suave: acelera al empezar y frena al terminar
        float target = busy ? 320f : 0f;
        speed += (target - speed) * Math.min(1f, dt * (busy ? 2.5f : 1.8f));
        if (!busy && speed < 3f) speed = 0f;
        angle = (angle + speed * dt) % 360f;
        float activity = speed / 320f;
        phase += dt * 4f;

        // brillo de fondo
        float glow = 0.55f + activity * (0.35f + 0.10f * (float) Math.sin(phase));
        haloPaint.setAlpha((int) (255 * Math.min(1f, glow)));
        c.drawCircle(cx, cy, radius, haloPaint);

        // anillo exterior
        c.drawCircle(cx, cy, radius * 0.86f, ringPaint);

        // arcos de "cargando": solo aparecen al girar
        if (activity > 0.02f) {
            arcPaint.setAlpha((int) (230 * activity));
            float base = angle * 1.6f;
            for (int k = 0; k < 3; k++) {
                c.drawArc(arcRect, base + k * 120f, 42f, false, arcPaint);
            }
        }

        // trebol
        c.save();
        c.rotate(angle, cx, cy);
        c.drawPath(blades, bladePaint);
        c.drawPath(blades, bladeEdge);
        c.restore();
        c.drawCircle(cx, cy, coreR, corePaint);

        if (busy || speed > 0f) postInvalidateOnAnimation();
    }
}
