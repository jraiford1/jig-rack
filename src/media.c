/* Jig Rack -- the eject media jig.
 * Copyright (C) 2026 Jon Raiford. GPL-3.0-or-later; see LICENSE.
 *
 * The tray menu lists the drives that can be safely removed, each as its own "Eject" item: removable drives (USB sticks, memory
 * cards) and USB, FireWire or SD-attached disks. Each disk is one entry, with all of its drive letters, since
 * removing it removes them all. Ejecting is what "Safely Remove Hardware" does: ask Windows (Plug and Play) to
 * eject the disk's removable device, which first makes sure nothing has a file open on it and flushes it. A card in
 * a built-in reader, where the reader itself stays, is dismounted and its media ejected instead.
 * Ejecting runs on a worker thread (it can take a few seconds) and reports back with a notification. */
#include "jigrack.h"
#include <initguid.h>
#include <winioctl.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

/* ---------------------------------------------------------------- finding the drives -------------------------- */

static HANDLE open_volume(wchar_t letter, DWORD access)
{
    wchar_t path[8];
    swprintf_s(path, 8, L"\\\\.\\%c:", letter);
    return CreateFileW(path, access, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
}

static BOOL device_number(HANDLE h, STORAGE_DEVICE_NUMBER *n)
{
    DWORD got;
    return DeviceIoControl(h, IOCTL_STORAGE_GET_DEVICE_NUMBER, NULL, 0, n, sizeof *n, &got, NULL);
}

/* The disk's bus type and "vendor product" name, from the storage device descriptor. */
static int disk_describe(HANDLE h, wchar_t *name, int cch)
{
    STORAGE_PROPERTY_QUERY q = { StorageDeviceProperty, PropertyStandardQuery };
    BYTE buf[1024];
    DWORD got;
    name[0] = 0;
    if (!DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, buf, sizeof buf, &got, NULL) ||
        got < sizeof(STORAGE_DEVICE_DESCRIPTOR)) return BusTypeUnknown;
    const STORAGE_DEVICE_DESCRIPTOR *d = (const STORAGE_DEVICE_DESCRIPTOR *)buf;
    char text[200] = "";
    DWORD offs[2] = { d->VendorIdOffset, d->ProductIdOffset };
    for (int k = 0; k < 2; k++) {
        if (!offs[k] || offs[k] >= got) continue;
        char part[100];
        strncpy_s(part, sizeof part, (const char *)buf + offs[k], _TRUNCATE);
        char *s = part, *e;
        while (*s == ' ') s++;
        for (e = s + strlen(s); e > s && e[-1] == ' '; ) *--e = 0;
        if (!*s) continue;
        if (text[0]) strcat_s(text, sizeof text, " ");
        strcat_s(text, sizeof text, s);
    }
    MultiByteToWideChar(CP_ACP, 0, text, -1, name, cch);
    return d->BusType;
}

static void format_size(ULONGLONG bytes, wchar_t *out, int cch)
{
    double gb = bytes / 1e9;
    if (gb >= 1000) swprintf_s(out, cch, L"%.1f TB", gb / 1000);
    else if (gb >= 10) swprintf_s(out, cch, L"%.0f GB", gb);
    else if (gb >= 1) swprintf_s(out, cch, L"%.1f GB", gb);
    else swprintf_s(out, cch, L"%.0f MB", bytes / 1e6);
}

/* Menu text: "Eject &E: BACKUP - Samsung PSSD T7 (1.0 TB)"; '&' in a label is doubled so it isn't an accelerator. */
void media_label(Drive *d)
{
    wchar_t t[200] = L"Eject ", *p = t + 6;
    for (const wchar_t *s = d->letters; *s && p < t + 190; s++) {
        if (s == d->letters) *p++ = L'&';
        *p++ = *s;
    }
    *p = 0;
    if (d->volume[0]) { wcscat_s(t, 200, L" "); wcscat_s(t, 200, d->volume); }
    if (d->product[0]) { wcscat_s(t, 200, L" \x2014 "); wcscat_s(t, 200, d->product); }
    wchar_t size[32];
    format_size(d->bytes, size, 32);
    swprintf_s(d->label, ARRAYSIZE(d->label), L"%s (%s)", t, size);
    /* double any '&' after the accelerator (the one just before the first letter, at index 6) */
    wchar_t out[ARRAYSIZE(d->label)];
    int o = 0;
    for (int i = 0; d->label[i] && o < (int)ARRAYSIZE(out) - 2; i++) {
        out[o++] = d->label[i];
        if (d->label[i] == L'&' && i != 6) out[o++] = L'&';
    }
    out[o] = 0;
    wcscpy_s(d->label, ARRAYSIZE(d->label), out);
}

int media_list(Drive *out, int max)
{
    int n = 0;
    wchar_t windir[MAX_PATH] = L"C";
    GetWindowsDirectoryW(windir, MAX_PATH);
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(mask & (1u << i))) continue;
        wchar_t letter = (wchar_t)(L'A' + i), root[4] = { letter, L':', L'\\', 0 };
        if (towupper(windir[0]) == letter) continue;                  /* never the Windows drive */
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_REMOVABLE && type != DRIVE_FIXED) continue;
        HANDLE h = open_volume(letter, 0);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER num;
        wchar_t product[100];
        BOOL ok = device_number(h, &num);
        int bus = disk_describe(h, product, 100);
        CloseHandle(h);
        if (!ok) continue;
        if (type == DRIVE_FIXED && bus != BusTypeUsb && bus != BusType1394 && bus != BusTypeSd && bus != BusTypeMmc)
            continue;                                                 /* an internal disk */
        wchar_t volume[MAX_PATH + 1] = L"";
        ULARGE_INTEGER total = {0};
        if (!GetVolumeInformationW(root, volume, MAX_PATH + 1, NULL, NULL, NULL, NULL, 0)) continue;  /* no media */
        GetDiskFreeSpaceExW(root, NULL, &total, NULL);
        int k;
        for (k = 0; k < n; k++)
            if (out[k].devType == num.DeviceType && out[k].devNum == num.DeviceNumber) break;
        if (k == n) {
            if (n == max) continue;
            ZeroMemory(&out[n], sizeof out[n]);
            out[n].devType = num.DeviceType;
            out[n].devNum = num.DeviceNumber;
            out[n].removable = type == DRIVE_REMOVABLE;
            wcscpy_s(out[n].product, ARRAYSIZE(out[n].product), product);
            wcscpy_s(out[n].volume, ARRAYSIZE(out[n].volume), volume);
            n++;
        } else {
            out[k].volume[0] = 0;                                     /* several volumes: no one label fits */
        }
        Drive *d = &out[k];
        if (d->letters[0]) wcscat_s(d->letters, ARRAYSIZE(d->letters), L", ");
        wchar_t l[3] = { letter, L':', 0 };
        wcscat_s(d->letters, ARRAYSIZE(d->letters), l);
        d->letterMask |= 1u << i;
        d->bytes += total.QuadPart;
    }
    for (int k = 0; k < n; k++) media_label(&out[k]);
    return n;
}

/* ---------------------------------------------------------------- ejecting ------------------------------------ */

/* The disk's device node: the disk interface whose device number matches. */
static DEVINST disk_devinst(const Drive *d)
{
    DEVINST found = 0;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_DISK, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return 0;
    SP_DEVICE_INTERFACE_DATA ifd = { sizeof ifd };
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, NULL, &GUID_DEVINTERFACE_DISK, i, &ifd); i++) {
        BYTE buf[1024];
        SP_DEVICE_INTERFACE_DETAIL_DATA_W *det = (SP_DEVICE_INTERFACE_DETAIL_DATA_W *)buf;
        det->cbSize = sizeof *det;
        SP_DEVINFO_DATA dev = { sizeof dev };
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, sizeof buf, NULL, &dev)) continue;
        HANDLE h = CreateFileW(det->DevicePath, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        STORAGE_DEVICE_NUMBER num;
        if (device_number(h, &num) && num.DeviceType == d->devType && num.DeviceNumber == d->devNum) found = dev.DevInst;
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static void veto_text(PNP_VETO_TYPE veto, const wchar_t *name, wchar_t *why, int cch)
{
    if (veto == PNP_VetoOutstandingOpen || veto == PNP_VetoDevice)
        wcscpy_s(why, cch, L"a program still has a file open on it. Close it and try again.");
    else if (veto == PNP_VetoNonDisableable || veto == PNP_VetoIllegalDeviceRequest)
        wcscpy_s(why, cch, L"Windows won't let this drive be removed.");
    else
        swprintf_s(why, cch, L"Windows refused (veto %d%s%s).", (int)veto, name[0] ? L": " : L"", name);
}

/* A card in a built-in reader: lock and dismount each volume, then eject the media. */
static BOOL eject_media_only(const Drive *d, wchar_t *why, int cch)
{
    for (int i = 0; i < 26; i++) {
        if (!(d->letterMask & (1u << i))) continue;
        HANDLE h = open_volume((wchar_t)(L'A' + i), GENERIC_READ | GENERIC_WRITE);
        if (h == INVALID_HANDLE_VALUE) { swprintf_s(why, cch, L"couldn't open %c: (error %lu).", L'A' + i, GetLastError()); return FALSE; }
        DWORD got;
        BOOL locked = FALSE;
        for (int t = 0; t < 5 && !locked; t++) {
            locked = DeviceIoControl(h, FSCTL_LOCK_VOLUME, NULL, 0, NULL, 0, &got, NULL);
            if (!locked) Sleep(300);
        }
        if (!locked) {
            CloseHandle(h);
            wcscpy_s(why, cch, L"a program still has a file open on it. Close it and try again.");
            return FALSE;
        }
        PREVENT_MEDIA_REMOVAL pmr = { FALSE };
        DeviceIoControl(h, FSCTL_DISMOUNT_VOLUME, NULL, 0, NULL, 0, &got, NULL);
        DeviceIoControl(h, IOCTL_STORAGE_MEDIA_REMOVAL, &pmr, sizeof pmr, NULL, 0, &got, NULL);
        BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_EJECT_MEDIA, NULL, 0, NULL, 0, &got, NULL);
        DWORD err = GetLastError();
        CloseHandle(h);
        if (!ok) { swprintf_s(why, cch, L"the media couldn't be ejected (error %lu).", err); return FALSE; }
    }
    return TRUE;
}

BOOL media_eject(const Drive *d, wchar_t *why, int cch)
{
    why[0] = 0;
    DEVINST disk = disk_devinst(d);
    if (!disk) { wcscpy_s(why, cch, L"it's no longer connected."); return FALSE; }
    /* The nearest device, from the disk up, that Windows can remove (usually the USB mass storage device). */
    DEVINST node = disk, target = 0;
    for (int depth = 0; depth < 4 && node; depth++) {
        ULONG caps = 0, len = sizeof caps;
        if (CM_Get_DevNode_Registry_PropertyW(node, CM_DRP_CAPABILITIES, NULL, &caps, &len, 0) == CR_SUCCESS &&
            (caps & CM_DEVCAP_REMOVABLE)) { target = node; break; }
        DEVINST parent;
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        node = parent;
    }
    if (!target) {
        trace("eject %ls: no removable device above the disk; ejecting the media", d->letters);
        return eject_media_only(d, why, cch);
    }
    PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
    wchar_t name[MAX_PATH];
    for (int t = 0; t < 3; t++) {                      /* a just-closed file can take a moment to let go */
        veto = PNP_VetoTypeUnknown;
        name[0] = 0;
        CONFIGRET cr = CM_Request_Device_EjectW(target, &veto, name, MAX_PATH, 0);
        trace("eject %ls: CM_Request_Device_Eject -> cr %lu, veto %d %ls", d->letters, cr, (int)veto, name);
        if (cr == CR_SUCCESS && veto == PNP_VetoTypeUnknown) return TRUE;
        Sleep(500);
    }
    veto_text(veto, name, why, cch);
    return FALSE;
}

/* ---------------------------------------------------------------- the worker ---------------------------------- */

typedef struct { HWND notify; int n; Drive d[MAX_DRIVES]; } EjectJob;
static volatile LONG g_ejecting;

static DWORD WINAPI eject_thread(LPVOID p)
{
    EjectJob *job = (EjectJob *)p;
    EjectResult *r = (EjectResult *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *r);
    int good = 0;
    for (int i = 0; i < job->n; i++) {
        wchar_t why[200];
        if (media_eject(&job->d[i], why, 200)) { good++; continue; }
        if (r && !r->text[0]) {
            swprintf_s(r->text, ARRAYSIZE(r->text), L"%s couldn't be ejected: %s", job->d[i].letters, why);
        }
    }
    if (r) {
        r->ok = good == job->n;
        if (r->ok) {
            wcscpy_s(r->title, ARRAYSIZE(r->title), L"Safe to remove");
            if (job->n == 1) swprintf_s(r->text, ARRAYSIZE(r->text), L"%s %s can be removed now.", job->d[0].letters,
                                         job->d[0].product[0] ? job->d[0].product : L"");
            else swprintf_s(r->text, ARRAYSIZE(r->text), L"All %d drives can be removed now.", job->n);
        } else {
            wcscpy_s(r->title, ARRAYSIZE(r->title), good ? L"Some drives weren't ejected" : L"Not ejected");
        }
        if (!PostMessageW(job->notify, WM_EJECTED, 0, (LPARAM)r)) HeapFree(GetProcessHeap(), 0, r);
    }
    HeapFree(GetProcessHeap(), 0, job);
    InterlockedExchange(&g_ejecting, 0);
    return 0;
}

BOOL media_eject_async(HWND notify, const Drive *list, int n)
{
    if (n <= 0 || InterlockedCompareExchange(&g_ejecting, 1, 0)) return FALSE;
    EjectJob *job = (EjectJob *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof *job);
    if (!job) { g_ejecting = 0; return FALSE; }
    job->notify = notify;
    job->n = n > MAX_DRIVES ? MAX_DRIVES : n;
    CopyMemory(job->d, list, job->n * sizeof *list);
    HANDLE t = CreateThread(NULL, 0, eject_thread, job, 0, NULL);
    if (!t) { HeapFree(GetProcessHeap(), 0, job); g_ejecting = 0; return FALSE; }
    CloseHandle(t);
    return TRUE;
}
