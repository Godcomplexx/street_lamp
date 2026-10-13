"""Check the actual BLE link while reading optional firmware diagnostics."""
import argparse
import asyncio
import contextlib
import datetime
from pathlib import Path

import serial
from bleak import BleakClient, BleakScanner

SERVICE = "a7c00001-5a64-4b21-9d1c-49f39417a601"
STATE = "a7c00002-5a64-4b21-9d1c-49f39417a601"


async def diagnose(args, emit):
    port = serial.Serial(args.port, 115200, timeout=0) if args.port else None

    async def serial_log():
        while True:
            if port.in_waiting:
                emit(port.read(port.in_waiting).decode(errors="replace").rstrip())
            await asyncio.sleep(0.1)

    task = asyncio.create_task(serial_log()) if port else None
    try:
        if port:
            await asyncio.sleep(2)
            if args.power:
                port.write({"0": b"0", "-12": b"-", "9": b"+"}[args.power])
                await asyncio.sleep(1)
        results = await BleakScanner.discover(timeout=10, return_adv=True)
        matches = [(d, a) for d, a in results.values() if SERVICE in a.service_uuids]
        emit(f"Scanned {len(results)} devices; ESP32 matches: {len(matches)}")
        if not matches:
            return False
        device, advertisement = matches[0]
        emit(f"ESP32 address={device.address} name={advertisement.local_name} RSSI={advertisement.rssi}")
        try:
            async with BleakClient(device, timeout=20,
                                   winrt={"use_cached_services": False}) as client:
                state = list(await client.read_gatt_char(STATE))
                emit(f"CONNECTED={client.is_connected}; LED state={state}")
                return len(state) == 4
        except Exception as error:
            emit(f"CONNECT FAILED: {type(error).__name__}: {error}")
            return False
    finally:
        if task:
            task.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await task
        if port:
            port.close()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port")
    parser.add_argument("--power", choices=["0", "-12", "9"])
    parser.add_argument("--output", required=True)
    args = parser.parse_args()
    if args.power and not args.port:
        parser.error("--power requires --port and LED_DIAGNOSTICS firmware")
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as log:
        def emit(message):
            print(message, flush=True)
            log.write(message + "\n")
            log.flush()
        emit("UTC " + datetime.datetime.now(datetime.timezone.utc).isoformat())
        raise SystemExit(0 if asyncio.run(diagnose(args, emit)) else 1)
