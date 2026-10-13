package com.local.esp32led;

import android.annotation.SuppressLint;
import android.bluetooth.*;
import android.bluetooth.le.*;
import android.content.Context;
import android.os.Build;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelUuid;
import android.util.Log;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.UUID;

@SuppressLint("MissingPermission")
final class LedConnection {
    interface Listener {
        void onStatus(String message, boolean active, boolean ready);
        void onState(byte[] state);
    }
    static final UUID SERVICE = UUID.fromString("a7c00001-5a64-4b21-9d1c-49f39417a601");
    static final UUID STATE = UUID.fromString("a7c00002-5a64-4b21-9d1c-49f39417a601");
    private static final UUID CCCD = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb");
    // Public BLE address read from this user's ESP32-C3 during USB diagnostics.
    private static final String KNOWN_BOARD = "E8:3D:C1:9E:24:5A";
    private final Context context;
    private final Listener listener;
    private final Handler main = new Handler(Looper.getMainLooper());
    private BluetoothLeScanner scanner;
    private BluetoothGatt gatt;
    private BluetoothGattCharacteristic characteristic;
    private boolean scanning, ready, busy, retrying, backgroundAttempt, linkEstablished;
    private BluetoothDevice target;
    private int preferredPhy = BluetoothDevice.PHY_LE_1M_MASK;
    private byte[] pending;
    private final Runnable timeout = () -> {
        trace("Connection deadline; background=" + backgroundAttempt + "; linked=" + linkEstablished);
        if (!backgroundAttempt && !linkEstablished && target != null) retryConnection();
        else fail("Не удалось подключиться к ESP32. Повтори подключение");
    };
    private final Runnable retry = () -> {
        if (!retrying || target == null) return;
        retrying = false;
        openGatt(true);
    };
    private final Runnable scanTimeout = this::connectKnownBoard;
    private final Runnable writeTimeout = () -> fail("Связь потеряна. Подключись снова");
    private final Runnable flush = this::sendPending;

    LedConnection(Context context, Listener listener) { this.context = context; this.listener = listener; }
    boolean isActive() { return scanning || gatt != null || retrying; }
    private synchronized void trace(String message) {
        Log.d("LedConnection", message);
        // A single USB cable is shared by the phone and board. Keep the latest
        // diagnostics available after moving that cable; no network is used.
        File file = new File(context.getFilesDir(), "ble-debug.txt");
        try (FileOutputStream out = new FileOutputStream(file, file.length() < 32768)) {
            out.write((System.currentTimeMillis() + " " + message + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException error) { Log.w("LedConnection", "Cannot save diagnostics", error); }
    }
    void connect() {
        disconnect();
        trace("Connect requested; app=1.2; SDK=" + Build.VERSION.SDK_INT);
        try {
            BluetoothManager manager = context.getSystemService(BluetoothManager.class);
            BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
            if (adapter == null || !adapter.isEnabled()) { fail("Включи Bluetooth"); return; }
            trace("Adapter extended=" + adapter.isLeExtendedAdvertisingSupported()
                + "; PHY2M=" + adapter.isLe2MPhySupported() + "; PHYcoded=" + adapter.isLeCodedPhySupported());
            preferredPhy = BluetoothDevice.PHY_LE_1M_MASK;
            if (adapter.isLe2MPhySupported()) preferredPhy |= BluetoothDevice.PHY_LE_2M_MASK;
            if (adapter.isLeCodedPhySupported()) preferredPhy |= BluetoothDevice.PHY_LE_CODED_MASK;
            scanner = adapter.getBluetoothLeScanner();
            if (scanner == null) { fail("Bluetooth недоступен"); return; }
            scanning = true;
            listener.onStatus("Ищу ESP32 LED…", true, false);
            // Match service UUID or the firmware's advertised name in the callback.
            scanner.startScan(null,
                new ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY)
                    .setLegacy(false).setPhy(ScanSettings.PHY_LE_ALL_SUPPORTED).build(), scanCallback);
            main.postDelayed(scanTimeout, 8000);
        } catch (SecurityException | IllegalStateException e) { fail("Нет доступа к Bluetooth. Проверь разрешения"); }
    }
    private void connectKnownBoard() {
        if (!scanning) return;
        try {
            BluetoothManager manager = context.getSystemService(BluetoothManager.class);
            BluetoothAdapter adapter = manager == null ? null : manager.getAdapter();
            if (adapter == null || !adapter.isEnabled()) { fail("Включи Bluetooth"); return; }
            trace("Trying known ESP32 address after scanning");
            beginConnection(adapter.getRemoteDevice(KNOWN_BOARD));
        } catch (SecurityException | IllegalArgumentException e) {
            fail("Нет доступа к Bluetooth. Проверь разрешения");
        }
    }
    private void beginConnection(BluetoothDevice device) {
        stopScan();
        main.removeCallbacks(scanTimeout);
        main.removeCallbacks(timeout);
        target = device;
        linkEstablished = false;
        openGatt(false);
    }
    private void openGatt(boolean autoConnect) {
        backgroundAttempt = autoConnect;
        listener.onStatus(autoConnect ? "Повторно подключаюсь к ESP32 LED…" : "Подключаюсь к ESP32 LED…", true, false);
        try {
            trace("connectGatt address=" + target.getAddress() + "; autoConnect=" + autoConnect
                + "; preferredPhy=" + preferredPhy);
            gatt = target.connectGatt(context, autoConnect, callback,
                BluetoothDevice.TRANSPORT_LE, preferredPhy, main);
            if (gatt == null) { fail("Не удалось подключиться"); return; }
            main.postDelayed(timeout, autoConnect ? 45000 : 20000);
        } catch (SecurityException | IllegalArgumentException e) { fail("Не удалось подключиться"); }
    }
    private void retryConnection() {
        main.removeCallbacks(timeout);
        trace("Retry through autoConnect after direct connection failed");
        BluetoothGatt previous = gatt;
        gatt = null;
        closeGatt(previous);
        retrying = true;
        listener.onStatus("Повторно подключаюсь к ESP32 LED…", true, false);
        main.postDelayed(retry, 1000);
    }
    private void closeGatt(BluetoothGatt previous) {
        if (previous == null) return;
        try { previous.disconnect(); } catch (SecurityException ignored) { }
        try { previous.close(); } catch (SecurityException ignored) { }
    }
    private void stopScan() {
        if (scanning && scanner != null) {
            try { scanner.stopScan(scanCallback); } catch (SecurityException | IllegalStateException ignored) { }
        }
        scanning = false;
    }
    void disconnect() {
        main.removeCallbacks(timeout); main.removeCallbacks(scanTimeout);
        main.removeCallbacks(retry);
        main.removeCallbacks(writeTimeout); main.removeCallbacks(flush);
        stopScan(); ready = false; busy = false; pending = null; characteristic = null;
        retrying = false; backgroundAttempt = false; linkEstablished = false; target = null;
        BluetoothGatt previous = gatt; gatt = null;
        closeGatt(previous);
        trace("Disconnected/cancelled");
        listener.onStatus("Не подключено", false, false);
    }
    void destroy() { disconnect(); main.removeCallbacksAndMessages(null); }
    private void fail(String message) { trace("FAIL " + message); disconnect(); listener.onStatus(message, false, false); }

    private static int phyMask(int phy) {
        switch (phy) {
            case BluetoothDevice.PHY_LE_1M: return BluetoothDevice.PHY_LE_1M_MASK;
            case BluetoothDevice.PHY_LE_2M: return BluetoothDevice.PHY_LE_2M_MASK;
            case BluetoothDevice.PHY_LE_CODED: return BluetoothDevice.PHY_LE_CODED_MASK;
            default: return 0;
        }
    }

    private final ScanCallback scanCallback = new ScanCallback() {
        @Override public void onScanResult(int type, ScanResult result) {
            ScanRecord record = result.getScanRecord();
            String name = record == null ? null : record.getDeviceName();
            boolean serviceMatches = record != null && record.getServiceUuids() != null
                && record.getServiceUuids().contains(new ParcelUuid(SERVICE));
            if (!serviceMatches && !"ESP32 LED".equals(name) && !"ESP32LED".equals(name)
                    && !"C3EXT".equals(name)
                    && !KNOWN_BOARD.equals(result.getDevice().getAddress())) return;
            trace("Found ESP32 address=" + result.getDevice().getAddress() + "; name=" + name
                + "; RSSI=" + result.getRssi() + "; legacy=" + result.isLegacy()
                + "; connectable=" + result.isConnectable() + "; primaryPHY=" + result.getPrimaryPhy()
                + "; secondaryPHY=" + result.getSecondaryPhy() + "; dataStatus=" + result.getDataStatus());
            main.post(() -> {
                if (!scanning) return;
                final int advertisedPhy = phyMask(result.getPrimaryPhy()) | phyMask(result.getSecondaryPhy());
                if (advertisedPhy != 0) preferredPhy = advertisedPhy;
                beginConnection(result.getDevice());
            });
        }
        @Override public void onScanFailed(int error) {
            trace("Scan failed: " + error);
            main.post(() -> { if (scanning) connectKnownBoard(); });
        }
    };

    private final BluetoothGattCallback callback = new BluetoothGattCallback() {
        @Override public void onConnectionStateChange(BluetoothGatt source, int status, int state) {
            trace("Connection status=" + status + "; state=" + state);
            main.post(() -> {
                if (source != gatt) return;
                if (status != BluetoothGatt.GATT_SUCCESS || state == BluetoothProfile.STATE_DISCONNECTED) {
                    if (!linkEstablished && !backgroundAttempt && target != null
                            && (status == 147 || status == 133 || status == 8 || status == 62)) {
                        retryConnection(); return;
                    }
                    fail("ESP32 отключена. Подключись снова"); return;
                }
                if (state == BluetoothProfile.STATE_CONNECTED) {
                    linkEstablished = true;
                    trace("Link established; discovering services");
                    main.removeCallbacks(timeout);
                    main.postDelayed(timeout, 20000);
                    try { if (!source.discoverServices()) fail("Не удалось найти управление светом"); }
                    catch (SecurityException e) { fail("Нет разрешения Bluetooth"); }
                }
            });
        }
        @Override public void onServicesDiscovered(BluetoothGatt source, int status) {
            trace("Services status=" + status);
            main.post(() -> {
                if (source != gatt) return;
                BluetoothGattService service = source.getService(SERVICE);
                characteristic = service == null ? null : service.getCharacteristic(STATE);
                if (status != BluetoothGatt.GATT_SUCCESS || characteristic == null) { fail("Нужна прошивка ESP32 для этого приложения"); return; }
                try {
                    if (!source.setCharacteristicNotification(characteristic, true)) { fail("Не удалось включить синхронизацию"); return; }
                    BluetoothGattDescriptor descriptor = characteristic.getDescriptor(CCCD);
                    if (descriptor == null) { fail("Нет синхронизации состояния"); return; }
                    boolean started;
                    if (Build.VERSION.SDK_INT >= 33) started = source.writeDescriptor(descriptor, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) == BluetoothStatusCodes.SUCCESS;
                    else { descriptor.setValue(BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE); started = source.writeDescriptor(descriptor); }
                    if (!started) fail("Не удалось настроить подключение");
                } catch (SecurityException e) { fail("Нет разрешения Bluetooth"); }
            });
        }
        @Override public void onDescriptorWrite(BluetoothGatt source, BluetoothGattDescriptor descriptor, int status) {
            trace("Notification descriptor status=" + status);
            main.post(() -> {
                if (source != gatt) return;
                try {
                    if (status != BluetoothGatt.GATT_SUCCESS || !source.readCharacteristic(characteristic)) fail("Не удалось прочитать состояние света");
                } catch (SecurityException e) { fail("Нет разрешения Bluetooth"); }
            });
        }
        @Override public void onCharacteristicRead(BluetoothGatt source, BluetoothGattCharacteristic c, int status) {
            readState(source, c.getValue(), status);
        }
        @Override public void onCharacteristicRead(BluetoothGatt source, BluetoothGattCharacteristic c, byte[] value, int status) {
            readState(source, value, status);
        }
        @Override public void onCharacteristicChanged(BluetoothGatt source, BluetoothGattCharacteristic c) {
            changed(source, c.getValue());
        }
        @Override public void onCharacteristicChanged(BluetoothGatt source, BluetoothGattCharacteristic c, byte[] value) {
            changed(source, value);
        }
        @Override public void onCharacteristicWrite(BluetoothGatt source, BluetoothGattCharacteristic c, int status) {
            trace("Color write status=" + status);
            main.post(() -> {
                if (source != gatt) return;
                main.removeCallbacks(writeTimeout); busy = false;
                if (status != BluetoothGatt.GATT_SUCCESS) { fail("Не удалось передать цвет. Подключись снова"); return; }
                main.removeCallbacks(flush); main.post(flush);
            });
        }
    };
    private void readState(BluetoothGatt source, byte[] value, int status) {
        trace("LED read status=" + status + "; length=" + (value == null ? 0 : value.length));
        final byte[] copy = value == null ? null : value.clone();
        main.post(() -> {
            if (source != gatt) return;
            if (status != BluetoothGatt.GATT_SUCCESS || copy == null || copy.length != 4) { fail("Не удалось прочитать состояние света"); return; }
            main.removeCallbacks(timeout); ready = true;
            trace("READY: LED state read successfully");
            listener.onState(copy); listener.onStatus("ESP32 LED подключена", true, true);
        });
    }
    private void changed(BluetoothGatt source, byte[] value) {
        final byte[] copy = value == null ? null : value.clone();
        main.post(() -> { if (source == gatt && ready && !busy && pending == null && copy != null && copy.length == 4) listener.onState(copy); });
    }
    void send(byte[] state) {
        if (!ready || state.length != 4) return;
        pending = state.clone(); main.removeCallbacks(flush); main.postDelayed(flush, 40);
    }
    private void sendPending() {
        if (!ready || busy || pending == null || gatt == null || characteristic == null) return;
        byte[] value = pending; pending = null;
        try {
            boolean started;
            if (Build.VERSION.SDK_INT >= 33) started = gatt.writeCharacteristic(characteristic, value, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) == BluetoothStatusCodes.SUCCESS;
            else {
                characteristic.setWriteType(BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT);
                characteristic.setValue(value); started = gatt.writeCharacteristic(characteristic);
            }
            if (!started) { fail("Не удалось передать цвет. Повтори подключение"); return; }
            busy = true; main.postDelayed(writeTimeout, 4000);
        } catch (SecurityException e) { fail("Нет разрешения Bluetooth"); }
    }
}
