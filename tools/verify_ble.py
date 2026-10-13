"""Hardware integration check. Requires: pip install bleak."""
import asyncio
from bleak import BleakClient, BleakScanner

SERVICE = "a7c00001-5a64-4b21-9d1c-49f39417a601"
STATE = "a7c00002-5a64-4b21-9d1c-49f39417a601"


async def main():
    device = await BleakScanner.find_device_by_filter(
        lambda device, adv: SERVICE in adv.service_uuids, timeout=15
    )
    if not device:
        raise RuntimeError("ESP32 LED not found")
    print("Found:", device.address, device.name, flush=True)
    async with BleakClient(device, timeout=30, winrt={"use_cached_services": False}) as client:
        characteristic = client.services.get_characteristic(STATE)
        assert characteristic is not None
        assert any(d.uuid == "00002902-0000-1000-8000-00805f9b34fb" for d in characteristic.descriptors)
        original = bytes(await client.read_gatt_char(STATE))
        assert len(original) == 4
        events = asyncio.Queue()
        await client.start_notify(STATE, lambda _, data: events.put_nowait(bytes(data)))
        try:
            # Test arbitrary RGB, very dim, off, maximum brightness, and round trips.
            for packet in [bytes([17, 129, 238, 60]), bytes([255, 0, 0, 1]),
                           bytes([0, 255, 0, 0]), bytes([0, 0, 255, 255]),
                           bytes([255, 255, 255, 60])]:
                await client.write_gatt_char(STATE, packet, response=True)
                event = await asyncio.wait_for(events.get(), timeout=5)
                assert event == packet, (event, packet)
                actual = bytes(await client.read_gatt_char(STATE))
                assert actual == packet, (actual, packet)
                print("PASS RGB + brightness + notification:", list(packet), flush=True)
            await client.write_gatt_char(STATE, bytes([1, 2, 3]), response=True)
            await asyncio.sleep(0.2)
            assert bytes(await client.read_gatt_char(STATE)) == packet
            print("PASS malformed packet ignored", flush=True)
        finally:
            await client.write_gatt_char(STATE, original, response=True)
            await asyncio.sleep(0.2)
            assert bytes(await client.read_gatt_char(STATE)) == original
            await client.stop_notify(STATE)
            print("Restored original state:", list(original), flush=True)
    # Check that advertising resumes after disconnect.
    reconnected = await BleakScanner.find_device_by_filter(
        lambda _, adv: SERVICE in adv.service_uuids, timeout=10
    )
    assert reconnected is not None
    async with BleakClient(reconnected, timeout=30, winrt={"use_cached_services": False}) as client:
        assert len(await client.read_gatt_char(STATE)) == 4
    print("PASS disconnect and reconnect", flush=True)


if __name__ == "__main__":
    asyncio.run(main())
