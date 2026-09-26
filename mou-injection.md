# HID Injection — Kernel Mouse Injection via mouhid.sys
Date: 2026-05-07
Version: 2.0 (click support)

## What it does
A kernel-mode driver that injects synthetic mouse input directly into `mouhid.sys` by locating the internal `HidClassServiceCallback`, reading the device's `DeviceExtension`, and calling the callback at `DISPATCH_LEVEL` — bypassing the normal I/O stack and user-mode hooks. Supports relative/absolute movement and left/right button events.

## Architecture

### Core data structure
```cpp
// 24-byte structure matching mouhid.sys internal format
typedef struct {
    USHORT UnitId;
    USHORT Flags;           // MOUSE_MOVE_RELATIVE=0x0000, MOUSE_MOVE_ABSOLUTE=0x0001
    USHORT ButtonFlags;     // button event bitmask (see below)
    USHORT ButtonData;      // wheel delta (0 if unused)
    ULONG  RawButtons;
    LONG   LastX;
    LONG   LastY;
    ULONG  ExtraInformation;
} MOUSE_DATA, *PMOUSE_DATA;

// ButtonFlags values
#define MOUSE_LEFT_BUTTON_DOWN    0x0001
#define MOUSE_LEFT_BUTTON_UP      0x0002
#define MOUSE_RIGHT_BUTTON_DOWN   0x0004
#define MOUSE_RIGHT_BUTTON_UP     0x0008
#define MOUSE_MIDDLE_BUTTON_DOWN  0x0010
#define MOUSE_MIDDLE_BUTTON_UP    0x0020
```

### Injection flow
```
DriverEntry (DriverMain)
  → PsCreateSystemThread (WorkerThread)
      → FindMouHidModule()       // ZwQuerySystemInformation class 11
      → ScanHidCallback()        // byte pattern scan inside mouhid.sys
      → FindDeviceObject()       // ObReferenceObjectByName("\\Driver\\mouhid")
      → InspectDeviceExtension() // debug dump of critical DeviceExtension offsets
      → Polling loop (10 ms):
          → ReadCommandFile()    // C:\Windows\Temp\mouse_cmd.txt
          → ParseCommand()       // "MODE X Y"
          → dispatch action
          → DeleteCommandFile()
```

### DeviceExtension layout (mouhid.sys, Ghidra-analyzed, Windows 10.0.26100)
```
Offset  Field        Type    Notes
------  -----        ----    -----
+0x1C   state_flag   UINT32  must be 1 to accept input
+0xE0   device_ctx   PVOID   mouclass device pointer (callback first arg)
+0xE8   callback_ptr PVOID   MouseClassServiceCallback
```

### HidClassServiceCallback pattern (offset VA=0x3090)
```asm
48 89 4C 24 08     mov [rsp+8], rcx
55                 push rbp
53                 push rbx
56                 push rsi
57                 push rdi
41 54              push r12
41 55              push r13
```
13-byte exact match, no wildcards.

### Command file format
Path: `C:\Windows\Temp\mouse_cmd.txt`

```
<MODE> <X> <Y>
```

| MODE | Action | X / Y |
|------|--------|-------|
| `0`  | Relative move | delta pixels |
| `1`  | Absolute move | screen coords |
| `2`  | Left click (down+up) | ignored |
| `3`  | Right click (down+up) | ignored |
| `4`  | Left button down | ignored |
| `5`  | Left button up | ignored |
| `6`  | Right button down | ignored |
| `7`  | Right button up | ignored |
| `-1` | Stop thread | — |

Examples:
```
0 50 -30     → relative move +50 X, -30 Y
1 1920 1080  → absolute move to (1920, 1080)
2 0 0        → left click
3 0 0        → right click
```

## How to use it

### Loading the driver
```
D:\skills\dependencies\kdmapper_Release.exe D:\projects\hid-inject\x64\mou-inject.sys
```
Entry point `DriverMain` is called by KDMapper with `NULL, NULL`. It spawns a thread and returns immediately.

> Built output: `D:\projects\hid-inject\x64\mou-inject.sys`
> Loader: `D:\skills\dependencies\kdmapper_Release.exe`
> Reference (original Windows driver): `D:\skills\dependencies\mouhid.sys`

### Sending commands (PowerShell)

```powershell
# Relative move
Set-Content -Path "C:\Windows\Temp\mouse_cmd.txt" -Value "0 50 -30"

# Absolute move
Set-Content -Path "C:\Windows\Temp\mouse_cmd.txt" -Value "1 960 540"

# Left click
Set-Content -Path "C:\Windows\Temp\mouse_cmd.txt" -Value "2 0 0"

# Right click
Set-Content -Path "C:\Windows\Temp\mouse_cmd.txt" -Value "3 0 0"

# Stop
Set-Content -Path "C:\Windows\Temp\mouse_cmd.txt" -Value "-1 0 0"
```

### Helper script (move + click)

```powershell
$CMD = "C:\Windows\Temp\mouse_cmd.txt"

function Send-Mouse($value) {
    Set-Content -Path $CMD -Value $value -Encoding ASCII -NoNewline
    $dl = [DateTime]::UtcNow.AddSeconds(1)
    while ((Test-Path $CMD) -and [DateTime]::UtcNow -lt $dl) { Start-Sleep -Milliseconds 15 }
    Start-Sleep -Milliseconds 30
}

# Move and left-click at absolute position
Send-Mouse "1 960 540"   # move to center
Send-Mouse "2 0 0"       # left click

# Drag: left down → move → left up
Send-Mouse "4 0 0"
Send-Mouse "0 200 0"
Send-Mouse "5 0 0"
```

### API functions

| Function | Purpose |
|----------|---------|
| `FindMouHidModule()` | Locates `mouhid.sys` base and size via `ZwQuerySystemInformation` class 11 |
| `ScanHidCallback(Base, Size)` | Scans module bytes for the 13-byte callback prologue at VA 0x3090 |
| `FindDeviceObject()` | Resolves `\Driver\mouhid` → `DRIVER_OBJECT` → first `DEVICE_OBJECT` |
| `InspectDeviceExtension()` | Logs `+0x1C`, `+0xE0`, `+0xE8` for validation |
| `InjectMouse(X, Y, MoveFlags, ButtonFlags, ButtonData)` | Core injection: allocates MOUSE_DATA, raises IRQL, calls callback |
| `MoveRelative(X, Y)` | Wrapper: relative movement |
| `MoveAbsolute(X, Y)` | Wrapper: absolute movement |
| `ButtonEvent(downFlag, upFlag, label)` | Wrapper: press + 10ms delay + release |
| `ReadCommandFile(buf, size, bytesRead)` | Opens and reads `mouse_cmd.txt` |
| `DeleteCommandFile()` | Deletes `mouse_cmd.txt` after processing |
| `ParseCommand(buf, pMode, pX, pY)` | Parses `"MODE X Y"` into three LONG values |
| `WorkerThread(Context)` | Main loop: init → inspect → poll → dispatch |

## Pitfalls / Gotchas

- **[mouhid.sys not found]**: `strstr` matches `"mouhid"` in the full path. Verify with `!lm m mouhid` in WinDbg.
- **[Pattern scan fails]**: The 13-byte pattern is exact and version-specific (tested on Windows 10.0.26100). A Windows update may change the prologue. If scan fails, find `HidClassServiceCallback` in Ghidra and update the pattern bytes.
- **[state_flag == 0]**: `InjectMouse` returns `STATUS_UNSUCCESSFUL` if `DevExt+0x1C == 0`. The device isn't ready. Wait and retry.
- **[Crash on callback invocation]**: `MouseClassServiceCallback` must be called at `DISPATCH_LEVEL`. `KfRaiseIrql(DISPATCH_LEVEL)` is used before calling.
- **[DeviceExtension offsets undocumented]**: `+0xE0` and `+0xE8` were reverse-engineered via Ghidra. May differ between Windows builds. Always run `InspectDeviceExtension()` first to confirm non-NULL values.
- **[Absolute coordinates]**: For `MOUSE_MOVE_ABSOLUTE`, Windows expects coordinates normalized to `[0, 65535]`, NOT raw pixel values, unless the `MOUSE_VIRTUAL_DESKTOP` flag is also set. Raw pixel coords work on some systems but may be off on multi-monitor setups.
- **[Non-paged pool required]**: `MOUSE_DATA` uses `POOL_FLAG_NON_PAGED` — paged pool at `DISPATCH_LEVEL` causes `PAGE_FAULT_IN_NONPAGED_AREA`.
- **[Button + move in one packet]**: `InjectMouse` accepts both movement and button flags simultaneously. For a click-at-position: do two calls — one absolute move (buttons=0), then one click (X=0 Y=0 relative).
- **[File polling race condition]**: Write next command only after the previous file disappears. The PowerShell helper polls `Test-Path $CMD` for this reason.

## Global state
```cpp
PVOID g_MouHidBase   = NULL;  // mouhid.sys image base
ULONG g_MouHidSize   = 0;     // mouhid.sys image size
PVOID g_HidCallback  = NULL;  // HidClassServiceCallback address
PVOID g_DeviceObject = NULL;  // First mouhid DEVICE_OBJECT
```

## Full source code

```cpp
#include <ntddk.h>

// ============================================================================
// GLOBALS
// ============================================================================
PVOID g_MouHidBase   = NULL;
ULONG g_MouHidSize   = 0;
PVOID g_HidCallback  = NULL;
PVOID g_DeviceObject = NULL;

// ============================================================================
// SYSTEM MODULE STRUCTURES
// ============================================================================
typedef struct _SYSTEM_MODULE_ENTRY {
    HANDLE Section;
    PVOID  MappedBase;
    PVOID  ImageBase;
    ULONG  ImageSize;
    ULONG  Flags;
    USHORT LoadOrderIndex;
    USHORT InitOrderIndex;
    USHORT LoadCount;
    USHORT OffsetToFileName;
    UCHAR  FullPathName[256];
} SYSTEM_MODULE_ENTRY, *PSYSTEM_MODULE_ENTRY;

typedef struct _SYSTEM_MODULE_INFORMATION {
    ULONG               ModulesCount;
    SYSTEM_MODULE_ENTRY Modules[1];
} SYSTEM_MODULE_INFORMATION, *PSYSTEM_MODULE_INFORMATION;

extern "C" NTSTATUS NTAPI ZwQuerySystemInformation(
    _In_      ULONG  SystemInformationClass,
    _Inout_   PVOID  SystemInformation,
    _In_      ULONG  SystemInformationLength,
    _Out_opt_ PULONG ReturnLength
);

extern "C" NTSTATUS NTAPI ZwDeleteFile(_In_ POBJECT_ATTRIBUTES ObjectAttributes);

extern "C" NTSTATUS ObReferenceObjectByName(
    PUNICODE_STRING ObjectName,
    ULONG           Attributes,
    PACCESS_STATE   PassedAccessState,
    ACCESS_MASK     DesiredAccess,
    POBJECT_TYPE    ObjectType,
    KPROCESSOR_MODE AccessMode,
    PVOID           ParseContext,
    PVOID*          Object
);
extern "C" POBJECT_TYPE* IoDriverObjectType;

// ============================================================================
// MOUSE_INPUT_DATA — 24 bytes
// ============================================================================
#define MOUSE_MOVE_RELATIVE       0x0000
#define MOUSE_MOVE_ABSOLUTE       0x0001
#define MOUSE_LEFT_BUTTON_DOWN    0x0001
#define MOUSE_LEFT_BUTTON_UP      0x0002
#define MOUSE_RIGHT_BUTTON_DOWN   0x0004
#define MOUSE_RIGHT_BUTTON_UP     0x0008
#define MOUSE_MIDDLE_BUTTON_DOWN  0x0010
#define MOUSE_MIDDLE_BUTTON_UP    0x0020

typedef struct {
    USHORT UnitId;
    USHORT Flags;
    USHORT ButtonFlags;
    USHORT ButtonData;
    ULONG  RawButtons;
    LONG   LastX;
    LONG   LastY;
    ULONG  ExtraInformation;
} MOUSE_DATA, *PMOUSE_DATA;

typedef VOID(*PHID_SERVICE_CALLBACK)(
    PVOID       DeviceObject,
    PMOUSE_DATA InputDataStart,
    PMOUSE_DATA InputDataEnd,
    PULONG      InputDataConsumed
);

// ============================================================================
// UTILITIES
// ============================================================================
PCHAR GetFileName(PCHAR FullPath) {
    if (!FullPath) return (PCHAR)"<null>";
    PCHAR last = NULL, cur = FullPath;
    while (*cur) { if (*cur == '\\' || *cur == '/') last = cur; cur++; }
    return last ? last + 1 : FullPath;
}

// ============================================================================
// FIND mouhid.sys
// ============================================================================
VOID FindMouHidModule() {
    ULONG bufSize = 0;
    NTSTATUS status = ZwQuerySystemInformation(11, NULL, 0, &bufSize);
    if (status != STATUS_INFO_LENGTH_MISMATCH) return;

    PSYSTEM_MODULE_INFORMATION mods = (PSYSTEM_MODULE_INFORMATION)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, bufSize, 'MouF');
    if (!mods) return;

    status = ZwQuerySystemInformation(11, mods, bufSize, NULL);
    if (!NT_SUCCESS(status)) { ExFreePool(mods); return; }

    for (ULONG i = 0; i < mods->ModulesCount; i++) {
        PCHAR name = GetFileName((PCHAR)mods->Modules[i].FullPathName);
        if (strstr(name, "mouhid") != NULL) {
            g_MouHidBase = mods->Modules[i].ImageBase;
            g_MouHidSize = mods->Modules[i].ImageSize;
            DbgPrintEx(0, 0, "[mou] mouhid.sys base=0x%p size=0x%X\n",
                g_MouHidBase, g_MouHidSize);
            break;
        }
    }

    ExFreePool(mods);
    if (!g_MouHidBase) DbgPrintEx(0, 0, "[mou] mouhid.sys not found\n");
}

// ============================================================================
// PATTERN SCAN — HidClassServiceCallback prologue
// Confirmed at VA=0x3090 in mouhid.sys (Windows 10.0.26100)
// 13-byte exact match
// ============================================================================
PVOID ScanHidCallback(PVOID Base, ULONG Size) {
    UCHAR pattern[] = {
        0x48, 0x89, 0x4C, 0x24, 0x08,  // mov [rsp+8], rcx
        0x55,                           // push rbp
        0x53,                           // push rbx
        0x56,                           // push rsi
        0x57,                           // push rdi
        0x41, 0x54,                     // push r12
        0x41, 0x55                      // push r13
    };
    ULONG patSize = sizeof(pattern);

    if (!Base || Size < patSize) return NULL;

    PUCHAR p = (PUCHAR)Base;
    for (ULONG i = 0; i <= Size - patSize; i++) {
        BOOLEAN match = TRUE;
        for (ULONG j = 0; j < patSize; j++) {
            if (p[i + j] != pattern[j]) { match = FALSE; break; }
        }
        if (match) {
            DbgPrintEx(0, 0, "[mou] callback @ 0x%p (offset=0x%X)\n", (PVOID)(p + i), i);
            return (PVOID)(p + i);
        }
    }

    DbgPrintEx(0, 0, "[mou] callback not found\n");
    return NULL;
}

// ============================================================================
// FIND DEVICEOBJECT — \Driver\mouhid
// ============================================================================
NTSTATUS FindDeviceObject() {
    UNICODE_STRING driverName;
    RtlInitUnicodeString(&driverName, L"\\Driver\\mouhid");

    PDRIVER_OBJECT driverObj = NULL;
    NTSTATUS status = ObReferenceObjectByName(
        &driverName, OBJ_CASE_INSENSITIVE, NULL, 0,
        *IoDriverObjectType, KernelMode, NULL, (PVOID*)&driverObj);

    if (!NT_SUCCESS(status) || !driverObj) {
        DbgPrintEx(0, 0, "[mou] \\Driver\\mouhid not found: 0x%X\n", status);
        return STATUS_NOT_FOUND;
    }

    DbgPrintEx(0, 0, "[mou] DriverObject=0x%p\n", driverObj);

    PDEVICE_OBJECT dev = driverObj->DeviceObject;
    while (dev) {
        DbgPrintEx(0, 0, "[mou] Device=0x%p Type=%d\n", dev, dev->DeviceType);
        if (!g_DeviceObject) g_DeviceObject = dev;
        dev = dev->NextDevice;
    }

    ObDereferenceObject(driverObj);

    if (!g_DeviceObject) {
        DbgPrintEx(0, 0, "[mou] no device found\n");
        return STATUS_NOT_FOUND;
    }

    DbgPrintEx(0, 0, "[mou] using Device=0x%p\n", g_DeviceObject);
    return STATUS_SUCCESS;
}

// ============================================================================
// INSPECT DeviceExtension — validate offsets before injecting
// ============================================================================
VOID InspectDeviceExtension() {
    PDEVICE_OBJECT devObj = (PDEVICE_OBJECT)g_DeviceObject;
    PUCHAR devExt = (PUCHAR)devObj->DeviceExtension;
    if (!devExt) { DbgPrintEx(0, 0, "[mou] devExt NULL\n"); return; }

    DbgPrintEx(0, 0, "[mou] DevObj =0x%p\n",            devObj);
    DbgPrintEx(0, 0, "[mou] DevExt =0x%p\n",            devExt);
    DbgPrintEx(0, 0, "[mou] +0x1C state_flag=0x%08X\n", *(ULONG*)(devExt + 0x1C));
    DbgPrintEx(0, 0, "[mou] +0xE0 devCtx    =0x%p\n",   *(PVOID*)(devExt + 0xE0));
    DbgPrintEx(0, 0, "[mou] +0xE8 callback  =0x%p\n",   *(PVOID*)(devExt + 0xE8));
}

// ============================================================================
// CORE INJECT — movement + buttons in a single call
// ============================================================================
NTSTATUS InjectMouse(LONG X, LONG Y, USHORT MoveFlags, USHORT ButtonFlags, USHORT ButtonData) {
    if (!g_DeviceObject) return STATUS_INVALID_PARAMETER;

    PDEVICE_OBJECT devObj = (PDEVICE_OBJECT)g_DeviceObject;
    PUCHAR devExt = (PUCHAR)devObj->DeviceExtension;
    if (!devExt) return STATUS_INVALID_PARAMETER;

    if (*(ULONG*)(devExt + 0x1C) == 0) {
        DbgPrintEx(0, 0, "[mou] state_flag=0, skip\n");
        return STATUS_UNSUCCESSFUL;
    }

    PVOID              devCtx = *(PVOID*)(devExt + 0xE0);
    PHID_SERVICE_CALLBACK cb  = *(PHID_SERVICE_CALLBACK*)(devExt + 0xE8);

    if (!devCtx || !cb) {
        DbgPrintEx(0, 0, "[mou] devCtx or cb NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    PMOUSE_DATA pkt = (PMOUSE_DATA)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(MOUSE_DATA), 'MouI');
    if (!pkt) return STATUS_NO_MEMORY;

    RtlZeroMemory(pkt, sizeof(MOUSE_DATA));
    pkt->Flags       = MoveFlags;
    pkt->ButtonFlags = ButtonFlags;
    pkt->ButtonData  = ButtonData;
    pkt->LastX       = X;
    pkt->LastY       = Y;

    __try {
        ULONG consumed = 0;
        KIRQL oldIrql = KfRaiseIrql(DISPATCH_LEVEL);
        cb(devCtx, pkt, pkt + 1, &consumed);
        KeLowerIrql(oldIrql);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        DbgPrintEx(0, 0, "[mou] exception during inject\n");
        ExFreePool(pkt);
        return STATUS_UNSUCCESSFUL;
    }

    ExFreePool(pkt);
    return STATUS_SUCCESS;
}

// ============================================================================
// HELPERS
// ============================================================================
NTSTATUS MoveRelative(LONG X, LONG Y) {
    DbgPrintEx(0, 0, "[mou] move rel X=%ld Y=%ld\n", X, Y);
    return InjectMouse(X, Y, MOUSE_MOVE_RELATIVE, 0, 0);
}

NTSTATUS MoveAbsolute(LONG X, LONG Y) {
    DbgPrintEx(0, 0, "[mou] move abs X=%ld Y=%ld\n", X, Y);
    return InjectMouse(X, Y, MOUSE_MOVE_ABSOLUTE, 0, 0);
}

NTSTATUS ButtonEvent(USHORT downFlag, USHORT upFlag, PCHAR label) {
    LARGE_INTEGER delay; delay.QuadPart = -100000LL;
    DbgPrintEx(0, 0, "[mou] %s down\n", label);
    NTSTATUS s = InjectMouse(0, 0, MOUSE_MOVE_RELATIVE, downFlag, 0);
    if (!NT_SUCCESS(s)) return s;
    KeDelayExecutionThread(KernelMode, FALSE, &delay);
    DbgPrintEx(0, 0, "[mou] %s up\n", label);
    return InjectMouse(0, 0, MOUSE_MOVE_RELATIVE, upFlag, 0);
}

// ============================================================================
// READ / DELETE / PARSE COMMAND FILE
// ============================================================================
NTSTATUS ReadCommandFile(PCHAR buf, ULONG bufSize, PULONG bytesRead) {
    HANDLE hFile = NULL;
    UNICODE_STRING fn;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    RtlInitUnicodeString(&fn, L"\\??\\C:\\Windows\\Temp\\mouse_cmd.txt");
    InitializeObjectAttributes(&oa, &fn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    NTSTATUS st = ZwCreateFile(&hFile, GENERIC_READ, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) return st;

    st = ZwReadFile(hFile, NULL, NULL, NULL, &iosb, buf, bufSize - 1, NULL, NULL);
    if (NT_SUCCESS(st)) { buf[iosb.Information] = '\0'; *bytesRead = (ULONG)iosb.Information; }

    ZwClose(hFile);
    return st;
}

VOID DeleteCommandFile() {
    UNICODE_STRING fn;
    OBJECT_ATTRIBUTES oa;
    RtlInitUnicodeString(&fn, L"\\??\\C:\\Windows\\Temp\\mouse_cmd.txt");
    InitializeObjectAttributes(&oa, &fn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    ZwDeleteFile(&oa);
}

// Parse "MODE X Y"
NTSTATUS ParseCommand(PCHAR buf, PLONG pMode, PLONG pX, PLONG pY) {
    if (!buf || !buf[0]) return STATUS_INVALID_PARAMETER;
    PCHAR p = buf;

    INT s = (*p == '-') ? (p++, -1) : 1;
    LONG mode = 0;
    while (*p >= '0' && *p <= '9') mode = mode * 10 + (*p++ - '0');
    mode *= s;

    while (*p == ' ' || *p == '\t') p++;
    s = (*p == '-') ? (p++, -1) : 1;
    LONG x = 0;
    while (*p >= '0' && *p <= '9') x = x * 10 + (*p++ - '0');
    x *= s;

    while (*p == ' ' || *p == '\t') p++;
    s = (*p == '-') ? (p++, -1) : 1;
    LONG y = 0;
    while (*p >= '0' && *p <= '9') y = y * 10 + (*p++ - '0');
    y *= s;

    *pMode = mode; *pX = x; *pY = y;
    return STATUS_SUCCESS;
}

// ============================================================================
// WORKER THREAD
// ============================================================================
VOID WorkerThread(PVOID Context) {
    UNREFERENCED_PARAMETER(Context);

    DbgPrintEx(0, 0, "[mou] started\n");

    // Step 1: find mouhid.sys
    FindMouHidModule();
    if (!g_MouHidBase) { PsTerminateSystemThread(STATUS_UNSUCCESSFUL); return; }
    DbgPrintEx(0, 0, "[mou] step 1 ok\n");

    // Step 2: pattern scan for callback
    g_HidCallback = ScanHidCallback(g_MouHidBase, g_MouHidSize);
    if (!g_HidCallback) { PsTerminateSystemThread(STATUS_UNSUCCESSFUL); return; }
    DbgPrintEx(0, 0, "[mou] step 2 ok\n");

    // Step 3: find DeviceObject
    if (!NT_SUCCESS(FindDeviceObject())) { PsTerminateSystemThread(STATUS_UNSUCCESSFUL); return; }
    DbgPrintEx(0, 0, "[mou] step 3 ok\n");

    // Step 4: inspect DeviceExtension
    InspectDeviceExtension();
    DbgPrintEx(0, 0, "[mou] ready — C:\\Windows\\Temp\\mouse_cmd.txt\n");

    // Step 5: polling loop (10ms)
    CHAR  cmdBuf[32];
    ULONG bytesRead;
    LONG  mode, x, y;
    LARGE_INTEGER delay;
    delay.QuadPart = -100000LL;

    while (TRUE) {
        NTSTATUS st = ReadCommandFile(cmdBuf, sizeof(cmdBuf), &bytesRead);
        if (NT_SUCCESS(st) && bytesRead > 0) {
            if (NT_SUCCESS(ParseCommand(cmdBuf, &mode, &x, &y))) {
                DeleteCommandFile();

                switch (mode) {
                case -1: DbgPrintEx(0, 0, "[mou] stop\n"); goto done;
                case  0: MoveRelative(x, y); break;
                case  1: MoveAbsolute(x, y); break;
                case  2: ButtonEvent(MOUSE_LEFT_BUTTON_DOWN,  MOUSE_LEFT_BUTTON_UP,  (PCHAR)"LClick"); break;
                case  3: ButtonEvent(MOUSE_RIGHT_BUTTON_DOWN, MOUSE_RIGHT_BUTTON_UP, (PCHAR)"RClick"); break;
                case  4: DbgPrintEx(0, 0, "[mou] LDown\n");
                         InjectMouse(0, 0, MOUSE_MOVE_RELATIVE, MOUSE_LEFT_BUTTON_DOWN,  0); break;
                case  5: DbgPrintEx(0, 0, "[mou] LUp\n");
                         InjectMouse(0, 0, MOUSE_MOVE_RELATIVE, MOUSE_LEFT_BUTTON_UP,    0); break;
                case  6: DbgPrintEx(0, 0, "[mou] RDown\n");
                         InjectMouse(0, 0, MOUSE_MOVE_RELATIVE, MOUSE_RIGHT_BUTTON_DOWN, 0); break;
                case  7: DbgPrintEx(0, 0, "[mou] RUp\n");
                         InjectMouse(0, 0, MOUSE_MOVE_RELATIVE, MOUSE_RIGHT_BUTTON_UP,   0); break;
                default: DbgPrintEx(0, 0, "[mou] unknown mode %ld\n", mode); break;
                }
            }
        }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

done:
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ============================================================================
// ENTRY POINT — KDMapper calls this with (NULL, NULL)
// ============================================================================
extern "C" NTSTATUS DriverMain(
    _In_ PDRIVER_OBJECT   kdmapperParam1,
    _In_ PUNICODE_STRING  kdmapperParam2)
{
    UNREFERENCED_PARAMETER(kdmapperParam1);
    UNREFERENCED_PARAMETER(kdmapperParam2);

    HANDLE hThread = NULL;
    NTSTATUS status = PsCreateSystemThread(
        &hThread, THREAD_ALL_ACCESS,
        NULL, NULL, NULL, WorkerThread, NULL);

    if (!NT_SUCCESS(status)) {
        DbgPrintEx(0, 0, "[mou] thread failed: 0x%X\n", status);
        return status;
    }

    ZwClose(hThread);
    return STATUS_SUCCESS;
}
```

## KDMapper Integration

### Entry Point

| Item | Value |
|------|-------|
| **Function name** | `DriverMain` |
| **Signature** | `NTSTATUS DriverMain(PDRIVER_OBJECT, PUNICODE_STRING)` |
| **Linker setting** | Linker → Advanced → Entry Point: `DriverMain` |

### KDMapper Driver Rules Applied

| Rule | Compliance |
|------|-----------|
| **No `IoCreateDevice`** | ✅ All work done via `mouhid.sys` device objects |
| **No `DriverUnload`** | ✅ Fire-and-forget, no cleanup |
| **Fast entry point** | ✅ `DriverMain` spawns thread and returns immediately |
| **Uses `PsCreateSystemThread`** | ✅ `WorkerThread` handles all init and polling |
| **No NULL dereference** | ✅ Both KDMapper params are `UNREFERENCED_PARAMETER` |
| **Compile `/GS-`** | ✅ `<BufferSecurityCheck>false</BufferSecurityCheck>` in both configs |

### vcxproj Checklist

| Setting | Value |
|---------|-------|
| `EntryPointSymbol` | `DriverMain` |
| `BufferSecurityCheck` | `false` (Debug **and** Release) |
| `ConfigurationType` | `DynamicLibrary` |
| `PlatformToolset` | `v143` |
| `TargetExt` | `.sys` |
| `SubSystem` | `Native` |
| `Driver` | `WDM` |
| `NoDefaultLib` | `true` |
| `GenerateManifest` | `false` |
| `EmbedManifest` | `false` |
| `AdditionalDependencies` | `ntoskrnl.lib;hal.lib` |
| `AdditionalIncludeDirectories` | `C:\Program Files (x86)\Windows Kits\10\Include\10.0.28000.0\km` |
| `PreprocessorDefinitions` | `_AMD64_;_WIN64` |
| `WindowsTargetPlatformVersion` | `10.0.26100.0` |
| Build config | **Release x64** for mapping |

### Mapping Flow

```
User-mode process
  → KDMapper loads iqvw64e.sys (trusted signer)
  → KDMapper copies mouhid.sys bytes to kernel (skipping PE header)
  → KDMapper resolves imports and relocations
  → KDMapper calls DriverMain(NULL, NULL)
  → DriverMain spawns WorkerThread
  → WorkerThread:
      1. Finds mouhid.sys via ZwQuerySystemInformation class 11
      2. Scans for 13-byte HidClassServiceCallback prologue at VA 0x3090
      3. Resolves \Driver\mouhid -> DEVICE_OBJECT
      4. Validates DevExt offsets +0x1C / +0xE0 / +0xE8
      5. Enters 10ms polling loop for mouse_cmd.txt
```

### See Also

- [KDMapper skill documentation](kdmapper.md) — full rules, CLI parameters, and common errors
- [Keyboard injection](kbd-injection.md) — kbdhid.sys equivalent (keyboard version)
- [KDMapper GitHub](https://github.com/TheCruZ/kdmapper) — upstream project

### Mixed alternative

If you need **mouse and keyboard simultaneously** in a single driver, use `hid-inject.sys` instead:

```
D:\skills\dependencies\kdmapper_Release.exe D:\projects\hid-inject\x64\hid-inject.sys
```

It polls both `kbd_cmd.txt` and `mouse_cmd.txt` in the same 10ms loop — both channels are fully independent and can be written concurrently without blocking each other. See `D:\projects\hid-inject\combined\src\main.cpp`.
