/* Jig Rack -- Bluetooth battery levels, as Windows reports them.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE.
 *
 * Windows reads the battery level of paired Bluetooth devices (the LE Battery Service, or the hands-free profile on
 * classic devices) and keeps it as a device property, {104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2, a byte from 0 to 100,
 * on one of the device's nodes (BTHLE\..., BTHLEDevice\..., BTHENUM\...). Settings > Bluetooth & devices shows the
 * same number. Jig Rack lists every Bluetooth node that has it, named after the device it belongs to, and reads the
 * chosen node with the configuration manager. No WinRT, no Bluetooth API, nothing kept open; Windows gives no change
 * notification for it, so the monitor thread rereads it once a minute.
 */
#include "jigrack.h"
#include <initguid.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#pragma comment(lib, "cfgmgr32.lib")

static const DEVPROPKEY BT_BATTERY = { { 0x104ea319, 0x6ee2, 0x4701, { 0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5 } }, 2 };

static int node_battery(DEVINST d)
{
    DEVPROPTYPE t;
    BYTE v = 0;
    ULONG sz = sizeof v;
    if (CM_Get_DevNode_PropertyW(d, &BT_BATTERY, &t, &v, &sz, 0) == CR_SUCCESS && t == DEVPROP_TYPE_BYTE && v <= 100)
        return v;
    return -1;
}

static BOOL node_string(DEVINST d, const DEVPROPKEY *key, wchar_t *out, int cch)
{
    DEVPROPTYPE t;
    ULONG sz = (ULONG)(cch * sizeof(wchar_t));
    return CM_Get_DevNode_PropertyW(d, key, &t, (PBYTE)out, &sz, 0) == CR_SUCCESS && t == DEVPROP_TYPE_STRING && out[0];
}

/* The node for the device as a whole (BTHENUM\DEV_... or BTHLE\DEV_...): the node itself or an ancestor. */
static DEVINST device_node(DEVINST d)
{
    for (int depth = 0; depth < 8; depth++) {
        wchar_t id[MAX_DEVICE_ID_LEN];
        if (CM_Get_Device_IDW(d, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS) break;
        if (_wcsnicmp(id, L"BTHENUM\\DEV_", 12) == 0 || _wcsnicmp(id, L"BTHLE\\DEV_", 10) == 0) return d;
        DEVINST parent;
        if (CM_Get_Parent(&parent, d, 0) != CR_SUCCESS) break;
        d = parent;
    }
    return 0;
}

int bt_list_devices(DeviceList *out)
{
    ZeroMemory(out, sizeof *out);
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, NULL, CM_GETIDLIST_FILTER_PRESENT) != CR_SUCCESS || !len) return 0;
    wchar_t *ids = (wchar_t *)HeapAlloc(GetProcessHeap(), 0, len * sizeof(wchar_t));
    if (!ids) return 0;
    if (CM_Get_Device_ID_ListW(NULL, ids, len, CM_GETIDLIST_FILTER_PRESENT) == CR_SUCCESS) {
        DEVINST seen[24];
        int nseen = 0;
        for (wchar_t *id = ids; *id && out->count < (int)(sizeof out->d / sizeof out->d[0]); id += wcslen(id) + 1) {
            if (_wcsnicmp(id, L"BTH", 3) != 0) continue;                 /* Bluetooth-enumerated nodes only */
            DEVINST d;
            if (CM_Locate_DevNodeW(&d, id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
            int pct = node_battery(d);
            if (pct < 0) continue;
            DEVINST dev = device_node(d);
            BOOL dup = FALSE;
            for (int k = 0; k < nseen; k++) dup |= dev && seen[k] == dev;  /* one entry per device */
            if (dup) continue;
            if (dev && nseen < 24) seen[nseen++] = dev;
            FoundDevice *f = &out->d[out->count++];
            ZeroMemory(f, sizeof *f);
            f->type = -1;
            f->percent = pct;
            wcsncpy_s(f->id, 200, id, _TRUNCATE);
            wcscpy_s(f->via, 64, L"Bluetooth");
            if (!node_string(dev ? dev : d, &DEVPKEY_Device_FriendlyName, f->name, 64) &&
                !node_string(dev ? dev : d, &DEVPKEY_Device_DeviceDesc, f->name, 64))
                wcscpy_s(f->name, 64, L"Bluetooth device");
            trace("search: Bluetooth \"%ls\" battery %d%% (%ls)", f->name, pct, id);
        }
    }
    HeapFree(GetProcessHeap(), 0, ids);
    return out->count;
}

int bt_battery(const wchar_t *id)
{
    DEVINST d;
    if (!id[0] || CM_Locate_DevNodeW(&d, (DEVINSTID_W)id, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return -1;
    return node_battery(d);
}
