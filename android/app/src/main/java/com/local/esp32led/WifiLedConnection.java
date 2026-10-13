package com.local.esp32led;

import android.content.Context;
import android.net.ConnectivityManager;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.net.NetworkRequest;
import android.net.wifi.WifiNetworkSpecifier;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.util.Log;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/**
 * Talks to the board over its own Wi-Fi access point. Frames are 4 bytes
 * [R, G, B, brightness] in both directions; the board repeats its state
 * every 2 seconds, so silence means the link is gone.
 */
final class WifiLedConnection {
    interface Listener {
        void onStatus(String message, boolean active, boolean ready);
        void onState(byte[] state);
    }
    // Must match wifi_led.cpp in the firmware.
    static final String SSID = "ESP32-LED";
    static final String PASSWORD = "esp32led";
    private static final String HOST = "192.168.4.1";
    private static final int PORT = 4210;
    private static final int READ_TIMEOUT_MS = 6000;
    // Ignore echoes and heartbeats briefly after a local change so the
    // controls do not jump back while the user is dragging them.
    private static final long LOCAL_PRIORITY_MS = 600;

    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private final ConnectivityManager connectivity;
    private ExecutorService io;
    private ConnectivityManager.NetworkCallback callback;
    private Socket socket;
    private OutputStream out;
    private boolean active, opening, ready;
    private int session;
    private byte[] pending;
    private long lastLocalChange;
    private final Runnable flush = this::sendPending;
    private final Runnable networkTimeout = () -> fail(Build.VERSION.SDK_INT >= 29
        ? "Не удалось подключиться к Wi-Fi " + SSID + ". Проверь, что ESP32 включена"
        : "Подключи телефон к Wi-Fi " + SSID + " (пароль " + PASSWORD + ") в настройках");

    WifiLedConnection(Context context, Listener listener) {
        this.listener = listener;
        connectivity = context.getSystemService(ConnectivityManager.class);
    }
    boolean isActive() { return active; }

    void connect() {
        disconnect();
        if (connectivity == null) { fail("Wi-Fi недоступен"); return; }
        active = true;
        final int current = ++session;
        io = Executors.newSingleThreadExecutor();
        listener.onStatus("Подключаюсь к Wi-Fi " + SSID + "…", true, false);
        NetworkRequest.Builder request = new NetworkRequest.Builder()
            .addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET);
        if (Build.VERSION.SDK_INT >= 29) {
            // Android shows its own dialog and joins the board's network for this app only.
            request.setNetworkSpecifier(new WifiNetworkSpecifier.Builder()
                .setSsid(SSID).setWpa2Passphrase(PASSWORD).build());
        }
        callback = new ConnectivityManager.NetworkCallback() {
            @Override public void onAvailable(Network network) {
                main.post(() -> { if (current == session) openSocket(network, current); });
            }
            @Override public void onUnavailable() {
                main.post(() -> { if (current == session) networkTimeout.run(); });
            }
            @Override public void onLost(Network network) {
                main.post(() -> { if (current == session) fail("Wi-Fi ESP32 потерян. Подключись снова"); });
            }
        };
        try {
            connectivity.requestNetwork(request.build(), callback);
            main.postDelayed(networkTimeout, 60000);
        } catch (SecurityException | IllegalArgumentException e) {
            Log.w("WifiLedConnection", "Network request failed", e);
            callback = null;
            fail("Нет доступа к Wi-Fi. Проверь разрешения");
        }
    }

    private void openSocket(Network network, int current) {
        main.removeCallbacks(networkTimeout);
        if (opening) return;  // onAvailable may repeat for the same network
        opening = true;
        listener.onStatus("Подключаюсь к ESP32 LED…", true, false);
        io.execute(() -> {
            Socket created = null;
            try {
                created = network.getSocketFactory().createSocket();
                created.setTcpNoDelay(true);
                created.setSoTimeout(READ_TIMEOUT_MS);
                created.connect(new InetSocketAddress(HOST, PORT), 5000);
                final Socket opened = created;
                final OutputStream stream = opened.getOutputStream();
                final InputStream input = opened.getInputStream();
                main.post(() -> {
                    if (current != session) { closeQuietly(opened); return; }
                    socket = opened; out = stream;
                });
                new Thread(() -> readLoop(input, current), "led-reader").start();
            } catch (IOException e) {
                Log.w("WifiLedConnection", "Socket connect failed", e);
                closeQuietly(created);
                main.post(() -> { if (current == session) fail(Build.VERSION.SDK_INT >= 29
                    ? "ESP32 не отвечает. Повтори подключение"
                    : "ESP32 не отвечает. Проверь, что телефон подключён к Wi-Fi " + SSID); });
            }
        });
    }

    private void readLoop(InputStream input, int current) {
        byte[] frame = new byte[4];
        int length = 0;
        try {
            while (true) {
                int count = input.read(frame, length, frame.length - length);
                if (count < 0) throw new IOException("Closed by board");
                length += count;
                if (length < frame.length) continue;
                length = 0;
                final byte[] state = frame.clone();
                main.post(() -> onFrame(state, current));
            }
        } catch (IOException e) {
            main.post(() -> { if (current == session) fail("Связь с ESP32 потеряна. Подключись снова"); });
        }
    }

    private void onFrame(byte[] state, int current) {
        if (current != session) return;
        if (!ready) {
            // The board sends its state right after accepting the connection.
            ready = true;
            listener.onState(state);
            listener.onStatus("ESP32 LED подключена", true, true);
            return;
        }
        if (pending == null && SystemClock.uptimeMillis() - lastLocalChange > LOCAL_PRIORITY_MS)
            listener.onState(state);
    }

    void send(byte[] state) {
        if (!ready || state.length != 4) return;
        pending = state.clone();
        lastLocalChange = SystemClock.uptimeMillis();
        main.removeCallbacks(flush); main.postDelayed(flush, 40);
    }
    private void sendPending() {
        if (!ready || pending == null || out == null) return;
        final byte[] value = pending; pending = null;
        final OutputStream stream = out;
        final int current = session;
        io.execute(() -> {
            try { stream.write(value); stream.flush(); }
            catch (IOException e) {
                main.post(() -> { if (current == session) fail("Не удалось передать цвет. Подключись снова"); });
            }
        });
    }

    void disconnect() {
        session++;
        main.removeCallbacks(networkTimeout); main.removeCallbacks(flush);
        boolean wasActive = active;
        active = false; opening = false; ready = false; pending = null; out = null;
        Socket previous = socket; socket = null;
        ExecutorService executor = io; io = null;
        if (executor != null) {
            // Close off the main thread; closing also ends the reader thread.
            executor.execute(() -> closeQuietly(previous));
            executor.shutdown();
        }
        if (callback != null) {
            try { connectivity.unregisterNetworkCallback(callback); } catch (IllegalArgumentException ignored) { }
            callback = null;
        }
        if (wasActive) listener.onStatus("Не подключено", false, false);
    }
    void destroy() { disconnect(); main.removeCallbacksAndMessages(null); }
    private void fail(String message) { disconnect(); listener.onStatus(message, false, false); }
    private static void closeQuietly(Socket socket) {
        if (socket == null) return;
        try { socket.close(); } catch (IOException ignored) { }
    }
}
