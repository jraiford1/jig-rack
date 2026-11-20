"""Read a Logitech wireless mouse's battery over its receiver with HID++ 2.0 (read-only queries, stdlib only).

Tries UNIFIED_BATTERY (0x1004), then BATTERY_STATUS (0x1000), then BATTERY_VOLTAGE (0x1001) on receiver slots 1-6.
Prints one line ("49%, discharging [UNIFIED_BATTERY]"), or JSON with --json:
  {"ok": true, "percent": 49, "state": "discharging", "charging": false, "method": "UNIFIED_BATTERY", "approx": false}
Exit code 1 if nothing answered (mouse asleep or off, receiver unplugged).
Diagnostic companion to Jig Rack's mouse battery jig (same protocol, from Python).
"""
import ctypes
import ctypes.wintypes as wt
import json
import sys
import time

setupapi, hid, k32 = ctypes.windll.setupapi, ctypes.windll.hid, ctypes.windll.kernel32
JSON = "--json" in sys.argv


class GUID(ctypes.Structure):
    _fields_ = [("Data1", wt.DWORD), ("Data2", wt.WORD), ("Data3", wt.WORD), ("Data4", ctypes.c_ubyte * 8)]


class SP_DEVICE_INTERFACE_DATA(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("InterfaceClassGuid", GUID), ("Flags", wt.DWORD), ("Reserved", ctypes.c_void_p)]


class HIDP_CAPS(ctypes.Structure):
    _fields_ = [("Usage", wt.USHORT), ("UsagePage", wt.USHORT), ("InputReportByteLength", wt.USHORT),
                ("OutputReportByteLength", wt.USHORT), ("FeatureReportByteLength", wt.USHORT),
                ("Reserved", wt.USHORT * 17), ("rest", wt.USHORT * 10)]


class OVERLAPPED(ctypes.Structure):
    _fields_ = [("Internal", ctypes.c_void_p), ("InternalHigh", ctypes.c_void_p), ("Offset", wt.DWORD),
                ("OffsetHigh", wt.DWORD), ("hEvent", wt.HANDLE)]


k32.CreateFileW.restype = wt.HANDLE
k32.CreateEventW.restype = wt.HANDLE
setupapi.SetupDiGetClassDevsW.restype = ctypes.c_void_p
INVALID = wt.HANDLE(-1).value


def hid_paths():
    g = GUID()
    hid.HidD_GetHidGuid(ctypes.byref(g))
    h = setupapi.SetupDiGetClassDevsW(ctypes.byref(g), None, None, 0x12)  # DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    i = 0
    while True:
        d = SP_DEVICE_INTERFACE_DATA()
        d.cbSize = ctypes.sizeof(d)
        if not setupapi.SetupDiEnumDeviceInterfaces(ctypes.c_void_p(h), None, ctypes.byref(g), i, ctypes.byref(d)):
            break
        need = wt.DWORD()
        setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(h), ctypes.byref(d), None, 0, ctypes.byref(need), None)
        buf = ctypes.create_string_buffer(need.value)
        ctypes.cast(buf, ctypes.POINTER(wt.DWORD))[0] = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
        if setupapi.SetupDiGetDeviceInterfaceDetailW(ctypes.c_void_p(h), ctypes.byref(d), buf, need, None, None):
            yield ctypes.wstring_at(ctypes.addressof(buf) + 4)
        i += 1
    setupapi.SetupDiDestroyDeviceInfoList(ctypes.c_void_p(h))


def open_long_channel():
    """The HID++ long-report collection (vendor page 0xFF00, 20-byte reports) of a Logitech receiver."""
    for p in hid_paths():
        if "vid_046d" not in p.lower():
            continue
        h = k32.CreateFileW(p, 0xC0000000, 3, None, 3, 0x40000000, None)  # R/W, share R/W, OPEN_EXISTING, OVERLAPPED
        if h in (None, INVALID):
            continue
        pp = ctypes.c_void_p()
        caps = HIDP_CAPS()
        ok = hid.HidD_GetPreparsedData(wt.HANDLE(h), ctypes.byref(pp)) and \
            hid.HidP_GetCaps(pp, ctypes.byref(caps)) == 0x110000
        if pp:
            hid.HidD_FreePreparsedData(pp)
        if ok and caps.UsagePage == 0xFF00 and caps.InputReportByteLength == 20:
            return h
        k32.CloseHandle(wt.HANDLE(h))
    return None


def io(h, data, write):
    ev = k32.CreateEventW(None, True, False, None)
    ov = OVERLAPPED()
    ov.hEvent = ev
    buf = ctypes.create_string_buffer(bytes(data), 20) if write else ctypes.create_string_buffer(20)
    n = wt.DWORD()
    (k32.WriteFile if write else k32.ReadFile)(wt.HANDLE(h), buf, 20, ctypes.byref(n), ctypes.byref(ov))
    if k32.WaitForSingleObject(wt.HANDLE(ev), 1500) != 0:
        k32.CancelIo(wt.HANDLE(h))
        k32.CloseHandle(wt.HANDLE(ev))
        return None
    k32.GetOverlappedResult(wt.HANDLE(h), ctypes.byref(ov), ctypes.byref(n), False)
    k32.CloseHandle(wt.HANDLE(ev))
    return buf.raw[:n.value]


def call(h, dev, feat_idx, func, params=b""):
    tag = (func << 4) | 0x0A
    if io(h, (bytes([0x11, dev, feat_idx, tag]) + params).ljust(20, b"\0"), True) is None:
        return None
    end = time.time() + 2
    while time.time() < end:
        r = io(h, b"", False)
        if not r:
            return None
        if r[0] == 0x11 and r[1] == dev and r[2] == feat_idx and r[3] == tag:
            return r
        if r[0] == 0x11 and r[1] == dev and r[2] == 0xFF:  # HID++ 2.0 error reply
            return None
    return None


def feature_index(h, dev, feat):
    r = call(h, dev, 0x00, 0, bytes([feat >> 8, feat & 0xFF]))
    return r[4] if r and r[4] else None


def volt_to_pct(mv):  # rough discharge curve for Logitech Li-ion gaming mice
    for v, p in [(4186, 100), (4067, 90), (3989, 80), (3922, 70), (3859, 60), (3808, 50), (3770, 40),
                 (3734, 30), (3684, 20), (3621, 10), (3500, 5)]:
        if mv >= v:
            return p
    return 0


def report(percent, state, charging, method, approx=False):
    if JSON:
        print(json.dumps({"ok": True, "percent": percent, "state": state, "charging": charging,
                          "method": method, "approx": approx}))
    else:
        print(f"{'~' if approx else ''}{percent}%, {state} [{method}]")
    return 0


def fail(msg):
    print(json.dumps({"ok": False, "error": msg}) if JSON else msg)
    return 1


def main():
    h = open_long_channel()
    if not h:
        return fail("No Logitech receiver found")
    try:
        for dev in range(1, 7):
            i = feature_index(h, dev, 0x1004)
            if i:
                r = call(h, dev, i, 1)
                if r:
                    states = {0: "discharging", 1: "charging", 2: "charging (slow)", 3: "full", 4: "charging error"}
                    return report(r[4], states.get(r[6], f"state {r[6]}"), r[6] in (1, 2), "UNIFIED_BATTERY")
            i = feature_index(h, dev, 0x1000)
            if i:
                r = call(h, dev, i, 0)
                if r:
                    states = {0: "discharging", 1: "charging", 2: "almost full", 3: "full", 4: "charging (slow)",
                              5: "invalid battery", 6: "thermal error"}
                    return report(r[4], states.get(r[6], f"state {r[6]}"), r[6] in (1, 2, 4), "BATTERY_STATUS")
            i = feature_index(h, dev, 0x1001)
            if i:
                r = call(h, dev, i, 0)
                if r:
                    mv, charging = (r[4] << 8) | r[5], bool(r[6] & 0x80)
                    return report(volt_to_pct(mv), "charging" if charging else "discharging", charging,
                                  "BATTERY_VOLTAGE", approx=True)
        return fail("Mouse didn't answer (asleep or off?)")
    finally:
        k32.CloseHandle(wt.HANDLE(h))


if __name__ == "__main__":
    sys.exit(main())
