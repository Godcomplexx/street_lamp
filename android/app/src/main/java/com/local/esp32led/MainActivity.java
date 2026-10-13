package com.local.esp32led;

import android.Manifest;
import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.location.LocationManager;
import android.os.Build;
import android.os.Bundle;
import android.provider.Settings;
import android.view.Gravity;
import android.view.View;
import android.view.WindowInsets;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.TextView;
import java.util.ArrayList;
import java.util.Locale;

public class MainActivity extends Activity implements WifiLedConnection.Listener {
    private static final int PERMISSIONS = 10;
    private final int ink = Color.rgb(244, 244, 251), muted = Color.rgb(160, 164, 186);
    private int rgb = Color.RED, brightness = 60, savedBrightness = 60;
    private boolean syncing, ready;
    private TextView status, hex, level;
    private View swatch;
    private ColorWheel wheel;
    private SeekBar dimmer;
    private Button connect, power;
    private WifiLedConnection connection;
    private final ArrayList<View> controls = new ArrayList<>();

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        if (state != null) {
            rgb = state.getInt("rgb", Color.RED);
            brightness = state.getInt("brightness", 60);
            savedBrightness = state.getInt("savedBrightness", 60);
        }
        connection = new WifiLedConnection(this, this);
        buildUi();
        updateUi();
        setControlsEnabled(false);
    }

    private int dp(float value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private GradientDrawable background(int color, float radius) {
        GradientDrawable drawable = new GradientDrawable();
        drawable.setColor(color); drawable.setCornerRadius(dp(radius)); return drawable;
    }
    private TextView text(String value, int size, int color) {
        TextView view = new TextView(this); view.setText(value); view.setTextSize(size);
        view.setTextColor(color); return view;
    }
    private void space(LinearLayout parent, int height) {
        parent.addView(new View(this), new LinearLayout.LayoutParams(1, dp(height)));
    }
    private Button button(String value) {
        Button view = new Button(this); view.setText(value); view.setAllCaps(false);
        view.setTextSize(15); view.setTextColor(ink); view.setMinHeight(dp(48));
        view.setBackgroundTintList(ColorStateList.valueOf(Color.rgb(57, 49, 83)));
        return view;
    }

    private void buildUi() {
        ScrollView scroll = new ScrollView(this); scroll.setFillViewport(true);
        if (Build.VERSION.SDK_INT >= 35) {
            scroll.setOnApplyWindowInsetsListener((view, insets) -> {
                android.graphics.Insets bars = insets.getInsets(WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
                view.setPadding(bars.left, bars.top, bars.right, bars.bottom);
                return WindowInsets.CONSUMED;
            });
        }
        scroll.setBackgroundColor(Color.rgb(17, 19, 28));
        LinearLayout page = new LinearLayout(this); page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(24), dp(24), dp(24), dp(28)); scroll.addView(page);
        TextView title = text("Твой свет", 32, ink);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD); page.addView(title);
        space(page, 4); page.addView(text("Любой цвет. Твоя яркость.", 15, muted));
        space(page, 24);
        LinearLayout card = new LinearLayout(this); card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(16), dp(16), dp(16));
        card.setBackground(background(Color.rgb(29, 32, 46), 20));
        status = text("●  Не подключено", 15, muted); card.addView(status);
        space(card, 6); card.addView(text("Включи ESP32 — телефон подключится к её Wi-Fi " + WifiLedConnection.SSID, 13, muted));
        space(card, 10); connect = button("Подключить ESP32"); card.addView(connect);
        connect.setOnClickListener(v -> {
            if (connection.isActive()) connection.disconnect(); else requestConnection();
        });
        page.addView(card); space(page, 24);

        LinearLayout colorRow = new LinearLayout(this); colorRow.setGravity(Gravity.CENTER_VERTICAL);
        swatch = new View(this); swatch.setBackground(background(rgb, 16));
        colorRow.addView(swatch, new LinearLayout.LayoutParams(dp(52), dp(52)));
        LinearLayout colorText = new LinearLayout(this); colorText.setOrientation(LinearLayout.VERTICAL);
        colorText.setPadding(dp(16), 0, 0, 0);
        colorText.addView(text("Цвет", 13, muted)); hex = text("#FF0000", 22, ink);
        hex.setTypeface(Typeface.MONOSPACE, Typeface.BOLD); colorText.addView(hex);
        colorRow.addView(colorText, new LinearLayout.LayoutParams(0, -2, 1));
        Button edit = button("HEX"); edit.setContentDescription("Ввести цвет в формате HEX");
        colorRow.addView(edit); controls.add(edit); edit.setOnClickListener(v -> editHex());
        page.addView(colorRow); space(page, 16);

        wheel = new ColorWheel(this);
        wheel.setListener(color -> { rgb = color; updateUi(); send(); });
        page.addView(wheel, new LinearLayout.LayoutParams(-1, dp(280))); controls.add(wheel);
        TextView hint = text("Нажми на круг · белый — в центре", 13, muted);
        hint.setGravity(Gravity.CENTER); page.addView(hint); space(page, 16);
        LinearLayout presets = new LinearLayout(this);
        int[] colors = {Color.RED, 0xFFFFA000, 0xFFFFFF00, 0xFF00FF80, 0xFF0088FF, 0xFFBB00FF, Color.WHITE};
        String[] names = {"Красный", "Оранжевый", "Жёлтый", "Зелёный", "Синий", "Фиолетовый", "Белый"};
        for (int i = 0; i < colors.length; i++) {
            final int color = colors[i];
            TextView preset = text("●", 34, color); preset.setGravity(Gravity.CENTER);
            preset.setContentDescription(names[i]); preset.setFocusable(true);
            presets.addView(preset, new LinearLayout.LayoutParams(0, dp(48), 1));
            controls.add(preset);
            preset.setOnClickListener(v -> { rgb = color; updateUi(); send(); });
        }
        page.addView(presets); space(page, 20);

        LinearLayout lightRow = new LinearLayout(this); lightRow.setGravity(Gravity.CENTER_VERTICAL);
        lightRow.addView(text("Яркость", 20, ink), new LinearLayout.LayoutParams(0, -2, 1));
        level = text("24%", 20, 0xFFB7A2FF); lightRow.addView(level); page.addView(lightRow);
        dimmer = new SeekBar(this); dimmer.setMax(255);
        dimmer.setContentDescription("Яркость светодиода");
        page.addView(dimmer, new LinearLayout.LayoutParams(-1, dp(52))); controls.add(dimmer);
        dimmer.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            public void onProgressChanged(SeekBar bar, int value, boolean fromUser) {
                if (fromUser && !syncing) {
                    brightness = value;
                    if (value > 0) savedBrightness = value;
                    updateUi(); send();
                }
            }
            public void onStartTrackingTouch(SeekBar bar) { }
            public void onStopTrackingTouch(SeekBar bar) { send(); }
        });
        LinearLayout bounds = new LinearLayout(this);
        bounds.addView(text("Тусклее", 13, muted), new LinearLayout.LayoutParams(0, -2, 1));
        bounds.addView(text("Ярче", 13, muted)); page.addView(bounds); space(page, 20);
        power = button("Выключить свет"); controls.add(power); page.addView(power);
        power.setOnClickListener(v -> {
            if (brightness == 0) brightness = Math.max(1, savedBrightness);
            else { savedBrightness = brightness; brightness = 0; }
            updateUi(); send();
        });
        space(page, 20);
        TextView footer = text("Кнопка на ESP32 тоже переключает цвета", 13, muted);
        footer.setGravity(Gravity.CENTER); page.addView(footer);
        setContentView(scroll);
    }

    private void editHex() {
        EditText input = new EditText(this); input.setSingleLine(true);
        input.setText(String.format(Locale.US, "%06X", rgb & 0xFFFFFF)); input.selectAll();
        input.setHint("Например, FF8800"); input.setPadding(dp(24), dp(16), dp(24), dp(16));
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("Цвет #RRGGBB")
            .setView(input).setNegativeButton("Отмена", null).setPositiveButton("Применить", null).create();
        dialog.setOnShowListener(v -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(b -> {
            String value = input.getText().toString().trim().replaceFirst("^#", "");
            if (!value.matches("[0-9a-fA-F]{6}")) { input.setError("Нужно 6 символов: 0–9 и A–F"); return; }
            rgb = Color.parseColor("#" + value); updateUi(); send(); dialog.dismiss();
        }));
        dialog.show();
    }

    private void updateUi() {
        syncing = true;
        hex.setText(String.format(Locale.US, "#%06X", rgb & 0xFFFFFF));
        swatch.setBackground(background(rgb, 16)); wheel.setColor(rgb);
        level.setText(getString(R.string.brightness_percent, Math.round(brightness * 100f / 255f)));
        dimmer.setProgress(brightness);
        power.setText(brightness == 0 ? "Включить свет" : "Выключить свет");
        syncing = false;
    }
    private void send() {
        if (ready) connection.send(new byte[]{(byte)Color.red(rgb), (byte)Color.green(rgb), (byte)Color.blue(rgb), (byte)brightness});
    }
    private void setControlsEnabled(boolean value) {
        for (View view : controls) { view.setEnabled(value); view.setAlpha(value ? 1f : .45f); }
    }

    private void requestConnection() {
        // Joining the board's network needs nearby-devices access (Android 13+)
        // or location access (Android 10–12). Older versions join via settings.
        String permission = Build.VERSION.SDK_INT >= 33 ? Manifest.permission.NEARBY_WIFI_DEVICES
            : Build.VERSION.SDK_INT >= 29 ? Manifest.permission.ACCESS_FINE_LOCATION : null;
        if (permission != null && checkSelfPermission(permission) != PackageManager.PERMISSION_GRANTED) {
            requestPermissions(Build.VERSION.SDK_INT >= 33 ? new String[]{permission}
                : new String[]{permission, Manifest.permission.ACCESS_COARSE_LOCATION}, PERMISSIONS);
            return;
        }
        if (Build.VERSION.SDK_INT >= 29 && Build.VERSION.SDK_INT <= 32) {
            LocationManager location = getSystemService(LocationManager.class);
            if (location != null && !location.isLocationEnabled()) {
                new AlertDialog.Builder(this).setTitle("Включи геолокацию")
                    .setMessage("На Android 10–12 она нужна системе для поиска Wi-Fi сети ESP32. Приложение не определяет и не сохраняет твоё местоположение.")
                    .setPositiveButton("Настройки", (d, w) -> startActivity(new Intent(Settings.ACTION_LOCATION_SOURCE_SETTINGS)))
                    .setNegativeButton("Отмена", null).show(); return;
            }
        }
        connection.connect();
    }
    @Override public void onRequestPermissionsResult(int request, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(request, permissions, results);
        if (request == PERMISSIONS) {
            for (int result : results) if (result != PackageManager.PERMISSION_GRANTED) {
                onStatus("Разреши доступ к устройствам поблизости в настройках приложения", false, false); return;
            }
            if (results.length > 0) requestConnection();
        }
    }
    @Override public void onStatus(String message, boolean active, boolean connected) {
        ready = connected; status.setText(getString(R.string.connection_status, message));
        status.setTextColor(connected ? 0xFF85E0B7 : muted);
        connect.setText(connected ? "Отключиться" : active ? "Отменить подключение" : "Подключить ESP32");
        setControlsEnabled(connected);
    }
    @Override public void onState(byte[] state) {
        if (state.length != 4) return;
        rgb = Color.rgb(state[0] & 255, state[1] & 255, state[2] & 255);
        brightness = state[3] & 255;
        if (brightness > 0) savedBrightness = brightness;
        updateUi();
    }
    @Override protected void onSaveInstanceState(Bundle state) {
        super.onSaveInstanceState(state); state.putInt("rgb", rgb);
        state.putInt("brightness", brightness); state.putInt("savedBrightness", savedBrightness);
    }
    @Override protected void onStop() {
        // The system Wi-Fi dialog may cover the app while connecting; only a
        // finished connection is closed when the app goes to the background.
        if (ready) connection.disconnect();
        super.onStop();
    }
    @Override protected void onDestroy() { connection.destroy(); super.onDestroy(); }
}
