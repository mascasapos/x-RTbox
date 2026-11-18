package com.xrtbox.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Typeface;
import android.text.Editable;
import android.text.Layout;
import android.util.AttributeSet;
import android.util.TypedValue;
import android.widget.EditText;

/** Editor de codigo: monoespaciado, sin ajuste de linea y con numeros de linea. */
public class CodeEditText extends EditText {

    public interface CursorListener {
        void onCursorMoved(int line, int column);
    }

    private CursorListener cursorListener;
    private Paint gutterPaint;
    private Paint numberPaint;
    private Paint linePaint;
    private float gutterWidth;
    private float pad;

    public CodeEditText(Context context) {
        this(context, null);
    }

    public CodeEditText(Context context, AttributeSet attrs) {
        super(context, attrs);
        setTypeface(Typeface.MONOSPACE);
        setHorizontallyScrolling(true);
        float textSize = getTextSize();
        pad = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 6, getResources().getDisplayMetrics());
        gutterPaint = new Paint();
        gutterPaint.setColor(0xFF120A1C);
        linePaint = new Paint();
        linePaint.setColor(0xFF5A1F99);
        linePaint.setStrokeWidth(Math.max(1f, pad / 4f));
        numberPaint = new Paint(Paint.ANTI_ALIAS_FLAG);
        numberPaint.setTypeface(Typeface.MONOSPACE);
        numberPaint.setTextSize(textSize * 0.85f);
        numberPaint.setColor(0xFF8F6BB8);
        numberPaint.setTextAlign(Paint.Align.RIGHT);
        gutterWidth = numberPaint.measureText("0000") + pad * 2;
        setPadding((int) (gutterWidth + pad), getPaddingTop(), getPaddingRight(), getPaddingBottom());
    }

    public void setCursorListener(CursorListener l) {
        cursorListener = l;
        reportCursor();
    }

    @Override
    protected void onSelectionChanged(int selStart, int selEnd) {
        super.onSelectionChanged(selStart, selEnd);
        reportCursor();
    }

    private void reportCursor() {
        if (cursorListener == null) return;
        Editable t = getText();
        if (t == null) return;
        int pos = Math.max(0, Math.min(getSelectionStart(), t.length()));
        int line = 1, col = 1;
        for (int i = 0; i < pos; i++) {
            if (t.charAt(i) == '\n') {
                line++;
                col = 1;
            } else {
                col++;
            }
        }
        cursorListener.onCursorMoved(line, col);
    }

    /** Coloca el cursor al principio de la linea indicada (empezando en 1). */
    public void goToLine(int line) {
        Editable t = getText();
        if (t == null) return;
        int pos = 0;
        int current = 1;
        while (current < line && pos < t.length()) {
            if (t.charAt(pos) == '\n') current++;
            pos++;
        }
        requestFocus();
        setSelection(Math.min(pos, t.length()));
        bringPointIntoView(Math.min(pos, t.length()));
    }

    @Override
    protected void onDraw(Canvas canvas) {
        super.onDraw(canvas);
        if (gutterPaint == null) return;
        Layout layout = getLayout();
        if (layout == null) return;
        int sx = getScrollX();
        int sy = getScrollY();
        canvas.drawRect(sx, sy, sx + gutterWidth, sy + getHeight(), gutterPaint);
        canvas.drawLine(sx + gutterWidth, sy, sx + gutterWidth, sy + getHeight(), linePaint);
        int top = getExtendedPaddingTop();
        int first = layout.getLineForVertical(Math.max(0, sy - top));
        int last = layout.getLineForVertical(sy - top + getHeight());
        // Sin ajuste de linea, cada linea visual es una linea del codigo
        for (int i = first; i <= last && i < layout.getLineCount(); i++) {
            float y = layout.getLineBaseline(i) + top;
            canvas.drawText(String.valueOf(i + 1), sx + gutterWidth - pad, y, numberPaint);
        }
    }
}
