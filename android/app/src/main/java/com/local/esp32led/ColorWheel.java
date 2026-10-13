package com.local.esp32led;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RadialGradient;
import android.graphics.Shader;
import android.graphics.SweepGradient;
import android.view.MotionEvent;
import android.view.View;

public final class ColorWheel extends View {
    interface Listener { void onColor(int color); }
    private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final float[] hsv = {0, 1, 1};
    private Listener listener;
    private float cx, cy, radius;
    private Shader hueShader, whiteShader;
    public ColorWheel(Context context) {
        super(context); setContentDescription("Цветовой круг: выбери оттенок и насыщенность"); setFocusable(true);
    }
    void setListener(Listener value) { listener = value; }
    void setColor(int color) { Color.colorToHSV(color, hsv); invalidate(); }
    @Override protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        cx = w / 2f; cy = h / 2f; radius = Math.max(1, Math.min(w, h) / 2f - 16 * getResources().getDisplayMetrics().density);
        hueShader = new SweepGradient(cx, cy, new int[]{Color.RED, Color.YELLOW, Color.GREEN, Color.CYAN, Color.BLUE, Color.MAGENTA, Color.RED}, null);
        whiteShader = new RadialGradient(cx, cy, radius, Color.WHITE, Color.TRANSPARENT, Shader.TileMode.CLAMP);
    }
    @Override protected void onDraw(Canvas canvas) {
        paint.setStyle(Paint.Style.FILL); paint.setShader(hueShader); canvas.drawCircle(cx, cy, radius, paint);
        paint.setShader(whiteShader); canvas.drawCircle(cx, cy, radius, paint); paint.setShader(null);
        double angle = Math.toRadians(hsv[0]);
        float x = cx + (float)Math.cos(angle) * radius * hsv[1];
        float y = cy + (float)Math.sin(angle) * radius * hsv[1];
        float scale = getResources().getDisplayMetrics().density;
        paint.setColor(Color.BLACK); canvas.drawCircle(x, y, 11 * scale, paint);
        paint.setColor(Color.WHITE); canvas.drawCircle(x, y, 9 * scale, paint);
        paint.setColor(Color.HSVToColor(hsv)); canvas.drawCircle(x, y, 6 * scale, paint);
    }
    @Override public boolean onTouchEvent(MotionEvent event) {
        if (!isEnabled()) return false;
        if (event.getActionMasked() == MotionEvent.ACTION_CANCEL) { getParent().requestDisallowInterceptTouchEvent(false); return true; }
        if (event.getActionMasked() == MotionEvent.ACTION_DOWN || event.getActionMasked() == MotionEvent.ACTION_MOVE || event.getActionMasked() == MotionEvent.ACTION_UP) {
            getParent().requestDisallowInterceptTouchEvent(event.getActionMasked() != MotionEvent.ACTION_UP);
            float x = event.getX() - cx, y = event.getY() - cy;
            hsv[0] = (float)((Math.toDegrees(Math.atan2(y, x)) + 360) % 360);
            hsv[1] = Math.min(1, (float)Math.hypot(x, y) / radius); hsv[2] = 1;
            invalidate(); if (listener != null) listener.onColor(Color.HSVToColor(hsv));
            if (event.getActionMasked() == MotionEvent.ACTION_UP) performClick();
            return true;
        }
        return super.onTouchEvent(event);
    }
    @Override public boolean performClick() { super.performClick(); return true; }
}
