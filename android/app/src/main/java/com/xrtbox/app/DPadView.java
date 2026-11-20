package com.xrtbox.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.view.MotionEvent;
import android.view.View;

/**
 * Cruceta: izquierda/derecha cambian el tiempo por division y arriba/abajo la
 * tension por division. Mantener pulsado repite.
 */
public class DPadView extends View {

    public static final int LEFT = 0, RIGHT = 1, UP = 2, DOWN = 3;

    public interface Listener {
        void onPad(int dir);
    }

    private static final int NEON = 0xFFB026FF;
    private final Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint arrow = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint pressedPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint center = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path tri = new Path();
    private Listener listener;
    private int pressed = -1;

    private final Runnable repeat = new Runnable() {
        @Override
        public void run() {
            if (pressed < 0) return;
            if (listener != null) listener.onPad(pressed);
            postDelayed(this, 160);
        }
    };

    public DPadView(Context c) {
        super(c);
        init();
    }

    public DPadView(Context c, AttributeSet a) {
        super(c, a);
        init();
    }

    private float dp(float v) {
        return TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private void init() {
        bg.setColor(0xFF0D0714);
        ring.setColor(NEON);
        ring.setStyle(Paint.Style.STROKE);
        ring.setStrokeWidth(dp(2));
        arrow.setColor(NEON);
        pressedPaint.setColor(ScopeView.PHOSPHOR);
        center.setColor(0xFF3B1460);
    }

    public void setListener(Listener l) {
        listener = l;
    }

    @Override
    protected void onDraw(Canvas c) {
        float w = getWidth(), h = getHeight();
        float cx = w / 2, cy = h / 2, r = Math.min(w, h) / 2 - dp(3);
        c.drawCircle(cx, cy, r, bg);
        c.drawCircle(cx, cy, r, ring);
        c.drawCircle(cx, cy, r * 0.22f, center);
        float a = r * 0.26f;   // tamano de la flecha
        float d = r * 0.62f;   // distancia al centro
        for (int dir = 0; dir < 4; dir++) {
            tri.reset();
            switch (dir) {
                case LEFT:
                    tri.moveTo(cx - d - a * 0.6f, cy);
                    tri.lineTo(cx - d + a * 0.6f, cy - a);
                    tri.lineTo(cx - d + a * 0.6f, cy + a);
                    break;
                case RIGHT:
                    tri.moveTo(cx + d + a * 0.6f, cy);
                    tri.lineTo(cx + d - a * 0.6f, cy - a);
                    tri.lineTo(cx + d - a * 0.6f, cy + a);
                    break;
                case UP:
                    tri.moveTo(cx, cy - d - a * 0.6f);
                    tri.lineTo(cx - a, cy - d + a * 0.6f);
                    tri.lineTo(cx + a, cy - d + a * 0.6f);
                    break;
                default:
                    tri.moveTo(cx, cy + d + a * 0.6f);
                    tri.lineTo(cx - a, cy + d - a * 0.6f);
                    tri.lineTo(cx + a, cy + d - a * 0.6f);
                    break;
            }
            tri.close();
            c.drawPath(tri, dir == pressed ? pressedPaint : arrow);
        }
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        switch (e.getActionMasked()) {
            case MotionEvent.ACTION_DOWN: {
                float dx = e.getX() - getWidth() / 2f, dy = e.getY() - getHeight() / 2f;
                float r = Math.min(getWidth(), getHeight()) / 2f;
                if (dx * dx + dy * dy < (r * 0.2f) * (r * 0.2f)) return true;
                if (Math.abs(dx) > Math.abs(dy)) pressed = dx < 0 ? LEFT : RIGHT;
                else pressed = dy < 0 ? UP : DOWN;
                invalidate();
                if (listener != null) listener.onPad(pressed);
                removeCallbacks(repeat);
                postDelayed(repeat, 450);
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                pressed = -1;
                removeCallbacks(repeat);
                invalidate();
                return true;
            default:
                return true;
        }
    }
}
