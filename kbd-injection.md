# HID Injection — Kernel Keyboard Injection via kbdhid.sys
Date: 2026-05-07
Version: 1.0 (Windows Kernel Mode Driver)

## What it does
A kernel-mode driver that injects synthetic keystrokes directly into `kbdhid.sys` by locating the internal `KeyboardClassServiceCallback`, reading the device's `DeviceExtension`, and calling the callback at `DISPATCH_LEVEL` — bypassing the normal I/O stack and user-mode hooks.

## Architecture

### Core data structure
```cpp
// 10-byte structure matching kbdhid.sys internal format
typedef struct {
    USHORT UnitId;
    USHORT MakeCode;          // PS/2 scan code (Set 1)
    USHORT Flags;             // 0x0000=KEY_MAKE, 0x0001=KEY_BREAK, 0x0002=KEY_E0, 0x0004=KEY_E1
    USHORT Reserved;
    ULONG  ExtraInformation;
} KBD_DATA, *PKBD_DATA;
```

### Injection flow
```
DriverEntry (DriverMain)
  → PsCreateSystemThread (WorkerThread)
      → FindKbdHidModule()       // ZwQuerySystemInformation class 11
      → ScanKbdCallback()        // byte pattern scan inside kbdhid.sys
      → FindDeviceObject()       // ObReferenceObjectByName("\\Driver\\kbdhid")
      → InspectDeviceExtension() // debug dump of critical DeviceExtension offsets
      → Polling loop (10 ms):
          → ReadCommandFile()    // C:\Windows\Temp\kbd_cmd.txt
          → ParseCommand()       // "SCANCODE MODE"
          → InjectKey()
          → DeleteCommandFile()
```

### DeviceExtension layout (kbdhid.sys, Ghidra-analyzed, Windows 10.0.26100)
```
Offset  Field        Type    Notes
------  -----        ----    -----
+0x1C   state_flag   UINT32  must be 1 to accept input
+0x88   device_ctx   PVOID   kbdclass device pointer (KeyboardClassServiceCallback first arg)
+0x90   callback_ptr PVOID   KeyboardClassServiceCallback
```

### KeyboardClassServiceCallback pattern (offset VA=0x35C0)
```asm
48 8B C4           mov  rax, rsp
48 89 58 08        mov  [rsp+8],  rbx
48 89 68 10        mov  [rsp+10], rbp
48 89 70 20        mov  [rsp+20], rsi
57                 push rdi
41 54              push r12
41 55              push r13
41 56              push r14
41 57              push r15
48 83 EC 30        sub  rsp, 30h
```
29-byte exact match, no wildcards.

> **kbdhid.sys quirk**: `FileAlignment == SectionAlignment == 0x1000`, so file offsets and VA offsets are identical — the raw byte at file offset `0x35C0` is exactly the function prologue. No section mapping adjustment needed.

### Command file format
Path: `C:\Windows\Temp\kbd_cmd.txt`

```
<SCANCODE> <MODE>
```

Scan code is accepted as **decimal** (e.g., `30`) or **hex** with `0x` prefix (e.g., `0x1E`).

| MODE | Meaning |
|------|---------|
| `0`  | Press + Release (full keystroke) |
| `1`  | Press only (key down) |
| `2`  | Release only (key up) |
| `-1` | Stop the worker thread |

Example: `0x1E 0` → press and release scan code `0x1E` (key 'A').

### PS/2 Scan Code Table (Set 1, common keys)
```
0x01 Esc    0x10 Q    0x1E A    0x2C Z    0x39 Space
0x02 1      0x11 W    0x1F S    0x2D X    0x3B F1
0x03 2      0x12 E    0x20 D    0x2E C    0x3C F2
0x04 3      0x13 R    0x21 F    0x2F V    0x1C Enter
0x05 4      0x14 T    0x22 G    0x30 B    0x0E Backspace
0x06 5      0x15 Y    0x23 H    0x31 N    0x2A LShift
0x07 6      0x16 U    0x24 J    0x32 M    0x36 RShift
0x08 7      0x17 I    0x25 K    0x38 LAlt 0x1D LCtrl
0x09 8      0x18 O    0x26 L    0x0F Tab
0x0A 9      0x19 P
```

Extended keys (e.g., arrow keys, numpad ins/del) require `Flags |= 0x0002` (KEY_E0).

## How to use it

### Prerequisites
- `kbdhid.sys` loaded (present on all Windows systems with USB/PS2 keyboard)
- DebugView or WinDbg for kernel log output
- KDMapper for loading (no signed driver required)

### Loading the driver
```
D:\skills\dependencies\kdmapper_Release.exe D:\projects\hid-inject\x64\kbd-inject.sys
```
The entry point `DriverMain` is called by KDMapper with `NULL, NULL` parameters. It spawns a system thread and returns immediately.

> Built output: `D:\projects\hid-inject\x64\kbd-inject.sys`
> Loader: `D:\skills\dependencies\kdmapper_Release.exe`
> Reference (original Windows driver): `D:\skills\dependencies\kbdhid.sys`

### Sending commands (user-mode)

Write a text file to `C:\Windows\Temp\kbd_cmd.txt`. The kernel thread polls every 10 ms, reads the file, injects the key event, then deletes the file.

```powershell
# Full keystroke (press + release) — key 'A' (0x1E)
Set-Content -Path "C:\Windows\Temp\kbd_cmd.txt" -Value "0x1E 0"

# Key down only — Left Shift (0x2A)
Set-Content -Path "C:\Windows\Temp\kbd_cmd.txt" -Value "0x2A 1"

# Key up only — Left Shift
Set-Content -Path "C:\Windows\Temp\kbd_cmd.txt" -Value "0x2A 2"

# Stop the injection thread
Set-Content -Path "C:\Windows\Temp\kbd_cmd.txt" -Value "0 -1"
```

### Typing a phrase (PowerShell helper)

```powershell
$CMD = "C:\Windows\Temp\kbd_cmd.txt"

$map = @{
    'a'=0x1E; 'b'=0x30; 'c'=0x2E; 'd'=0x20; 'e'=0x12
    'f'=0x21; 'g'=0x22; 'h'=0x23; 'i'=0x17; 'j'=0x24
    'k'=0x25; 'l'=0x26; 'm'=0x32; 'n'=0x31; 'o'=0x18
    'p'=0x19; 'q'=0x10; 'r'=0x13; 's'=0x1F; 't'=0x14
    'u'=0x16; 'v'=0x2F; 'w'=0x11; 'x'=0x2D; 'y'=0x15
    'z'=0x2C; ' '=0x39
}
$LSHIFT = 0x2A

function Send-Key($sc, $mode) {
    Set-Content -Path $CMD -Value "$sc $mode" -Encoding ASCII -NoNewline
    $dl = [DateTime]::UtcNow.AddSeconds(1)
    while ((Test-Path $CMD) -and [DateTime]::UtcNow -lt $dl) { Start-Sleep -Milliseconds 15 }
    Start-Sleep -Milliseconds 40
}

function Send-Char($ch) {
    $lower = $ch.ToString().ToLower()
    $isUpper = ($ch -cmatch '[A-Z]')
    if (-not $map.ContainsKey($lower)) { return }
    $sc = $map[$lower]
    if ($isUpper) {
        Send-Key $LSHIFT 1; Send-Key $sc 0; Send-Key $LSHIFT 2
    } else {
        Send-Key $sc 0
    }
}

foreach ($ch in "hello world".ToCharArray()) { Send-Char $ch }
```

### WASD spin example (5 seconds)

```powershell
$CMD = "C:\Windows\Temp\kbd_cmd.txt"
$keys = @(0x11, 0x1E, 0x1F, 0x20)  # W, A, S, D
$end = [DateTime]::UtcNow.AddSeconds(5)
$i = 0
while ([DateTime]::UtcNow -lt $end) {
    $sc = $keys[$i % 4]
    Set-Content -Path $CMD -Value "$sc 1" -Encoding ASCII -NoNewline  # press
    Start-Sleep -Milliseconds 80
    Set-Content -Path $CMD -Value "$sc 2" -Encoding ASCII -NoNewline  # release
    Start-Sleep -Milliseconds 30
    $i++
}
```

### API functions

| Function | Purpose |
|----------|---------|
| `FindKbdHidModule()` | Locates `kbdhid.sys` base address and size via `ZwQuerySystemInformation` class 11 |
| `ScanKbdCallback(Base, Size)` | Scans module bytes for the 29-byte prologue pattern at VA 0x35C0 |
| `FindDeviceObject()` | Resolves `\Driver\kbdhid` → `DRIVER_OBJECT` → first `DEVICE_OBJECT` |
| `InspectDeviceExtension()` | Logs `+0x1C`, `+0x88`, `+0x90` for validation before injection |
| `InjectKey(ScanCode, Flags)` | Core injection: allocates KBD_DATA, raises IRQL, calls callback |
| `ReadCommandFile(buf, size, bytesRead)` | Opens and reads `kbd_cmd.txt` |
| `DeleteCommandFile()` | Deletes `kbd_cmd.txt` after processing |
| `ParseCommand(buf, pScan, pMode)` | Parses `"SCANCODE MODE"` — accepts decimal or `0x` hex scan code |
| `WorkerThread(Context)` | Main loop: init → inspect → poll → inject |

## Pitfalls / Gotchas

- **[kbdhid.sys not found]**: The `strstr` check matches `"kbdhid"` in the full path. Verify with `!lm m kbdhid` in WinDbg. On systems without a physical keyboard (e.g., some VMs), `kbdhid.sys` may not be loaded.
- **[Pattern scan fails]**: The 29-byte pattern is exact and version-specific (tested on Windows 10.0.26100 / 10.0.22621). A Windows update may change the prologue. If scan fails, open `kbdhid.sys` in Ghidra, find `FUN_1c00035c0`, and update the pattern bytes.
- **[state_flag == 0]**: `InjectKey` reads `DevExt+0x1C` and returns `STATUS_UNSUCCESSFUL` if it's zero. This indicates the device isn't ready. Wait and retry.
- **[Crash on callback invocation]**: `KeyboardClassServiceCallback` must be called at `DISPATCH_LEVEL`. Calling at `PASSIVE_LEVEL` may cause a bugcheck. `KfRaiseIrql(DISPATCH_LEVEL)` is used before calling.
- **[DeviceExtension offsets are undocumented]**: Offsets `+0x88` (devCtx) and `+0x90` (callback) were reverse-engineered via Ghidra. They may differ between Windows builds. Always run `InspectDeviceExtension()` first to confirm non-NULL values.
- **[kbdhid vs kbdclass]**: `kbdhid.sys` is the HID keyboard minidriver. `kbdclass.sys` is the class driver that owns `KeyboardClassServiceCallback`. The callback pointer at `DevExt+0x90` points into `kbdclass.sys` — not into `kbdhid.sys` itself.
- **[Non-paged pool required]**: `KBD_DATA` is allocated with `POOL_FLAG_NON_PAGED` because the callback runs at `DISPATCH_LEVEL`. Using paged pool causes a `PAGE_FAULT_IN_NONPAGED_AREA` bugcheck.
- **[Extended keys need KEY_E0 flag]**: Arrow keys, Insert, Delete, Home, End, and numpad keys require `Flags |= 0x0002` in addition to `KEY_MAKE`/`KEY_BREAK`. Sending them with `Flags=0` produces wrong characters.
- **[Capital letters need Shift]**: The driver injects raw scan codes. To produce uppercase letters, send Left Shift press (`0x2A, Flags=0`), then the letter, then Left Shift release (`0x2A, Flags=1`).
- **[File polling race condition]**: The driver reads, processes, then deletes the file. Write a new command only after the previous file disappears. The PowerShell helper polls `Test-Path $CMD` for this reason.

## Global state
```cpp
PVOID g_KbdHidBase   = NULL;  // kbdhid.sys image base
ULONG g_KbdHidSize   = 0;     // kbdhid.sys image size
PVOID g_KbdCallback  = NULL;  // KeyboardClassServiceCallback address (inside kbdclass.sys)
PVOID g_DeviceObject = NULL;  // First kbdhid DEVICE_OBJECT
```

## Full source code

```cpp
#include <ntddk.h>

// ============================================================================
// GLOBALS
// ============================================================================
PVOID g_KbdHidBase   = NULL;
ULONG g_KbdHidSize   = 0;
PVOID g_KbdCallback  = NULL;
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
    ULONG             ModulesCount;
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
// KEYBOARD_INPUT_DATA — 10 bytes
// ============================================================================
typedef struct {
    USHORT UnitId;
    USHORT MakeCode;    // PS/2 scan code (Set 1)
    USHORT Flags;       // 0x0000=KEY_MAKE, 0x0001=KEY_BREAK, 0x0002=KEY_E0, 0x0004=KEY_E1
    USHORT Reserved;
    ULONG  ExtraInformation;
} KBD_DATA, *PKBD_DATA;

typedef VOID(*PKBD_SERVICE_CALLBACK)(
    PVOID     DeviceObject,
    PKBD_DATA InputDataStart,
    PKBD_DATA InputDataEnd,
    PULONG    InputDataConsumed
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
// FIND kbdhid.sys
// ============================================================================
VOID FindKbdHidModule() {
    ULONG bufSize = 0;
    NTSTATUS status = ZwQuerySystemInformation(11, NULL, 0, &bufSize);
    if (status != STATUS_INFO_LENGTH_MISMATCH) return;

    PSYSTEM_MODULE_INFORMATION mods = (PSYSTEM_MODULE_INFORMATION)ExAllocatePool2(
        POOL_FLAG_NON_PAGED, bufSize, 'KbdF');
    if (!mods) return;

    status = ZwQuerySystemInformation(11, mods, bufSize, NULL);
    if (!NT_SUCCESS(status)) { ExFreePool(mods); return; }

    for (ULONG i = 0; i < mods->ModulesCount; i++) {
        PCHAR name = GetFileName((PCHAR)mods->Modules[i].FullPathName);
        if (strstr(name, "kbdhid") != NULL) {
            g_KbdHidBase = mods->Modules[i].ImageBase;
            g_KbdHidSize = mods->Modules[i].ImageSize;
            DbgPrintEx(0, 0, "[kbd] kbdhid.sys base=0x%p size=0x%X\n",
                g_KbdHidBase, g_KbdHidSize);
            break;
        }
    }

    ExFreePool(mods);
    if (!g_KbdHidBase) DbgPrintEx(0, 0, "[kbd] kbdhid.sys not found\n");
}

// ============================================================================
// PATTERN SCAN — KeyboardClassServiceCallback invocation site
// Prologue confirmed at VA=0x35C0 in kbdhid.sys 10.0.26100
// (kbdhid has FileAlignment=SectionAlignment=0x1000, so file offset = VA offset)
// ============================================================================
PVOID ScanKbdCallback(PVOID Base, ULONG Size) {
    UCHAR pattern[] = {
        0x48, 0x8B, 0xC4,               // mov rax, rsp
        0x48, 0x89, 0x58, 0x08,         // mov [rsp+8],  rbx
        0x48, 0x89, 0x68, 0x10,         // mov [rsp+10], rbp
        0x48, 0x89, 0x70, 0x20,         // mov [rsp+20], rsi
        0x57,                           // push rdi
        0x41, 0x54,                     // push r12
        0x41, 0x55,                     // push r13
        0x41, 0x56,                     // push r14
        0x41, 0x57,                     // push r15
        0x48, 0x83, 0xEC, 0x30          // sub rsp, 30h
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
            DbgPrintEx(0, 0, "[kbd] callback @ 0x%p (offset=0x%X)\n", (PVOID)(p + i), i);
            return (PVOID)(p + i);
        }
    }

    DbgPrintEx(0, 0, "[kbd] callback not found\n");
    return NULL;
}

// ============================================================================
// FIND DEVICEOBJECT — \Driver\kbdhid
// ============================================================================
NTSTATUS FindDeviceObject() {
    UNICODE_STRING driverName;
    RtlInitUnicodeString(&driverName, L"\\Driver\\kbdhid");

    PDRIVER_OBJECT driverObj = NULL;
    NTSTATUS status = ObReferenceObjectByName(
        &driverName, OBJ_CASE_INSENSITIVE, NULL, 0,
        *IoDriverObjectType, KernelMode, NULL, (PVOID*)&driverObj);

    if (!NT_SUCCESS(status) || !driverObj) {
        DbgPrintEx(0, 0, "[kbd] \\Driver\\kbdhid not found: 0x%X\n", status);
        return STATUS_NOT_FOUND;
    }

    DbgPrintEx(0, 0, "[kbd] DriverObject=0x%p\n", driverObj);

    PDEVICE_OBJECT dev = driverObj->DeviceObject;
    while (dev) {
        DbgPrintEx(0, 0, "[kbd] Device=0x%p Type=%d\n", dev, dev->DeviceType);
        if (!g_DeviceObject) g_DeviceObject = dev;
        dev = dev->NextDevice;
    }

    ObDereferenceObject(driverObj);

    if (!g_DeviceObject) {
        DbgPrintEx(0, 0, "[kbd] no device found\n");
        return STATUS_NOT_FOUND;
    }

    DbgPrintEx(0, 0, "[kbd] using Device=0x%p\n", g_DeviceObject);
    return STATUS_SUCCESS;
}

// ============================================================================
// INSPECT DeviceExtension — validate offsets before injecting
// ============================================================================
VOID InspectDeviceExtension() {
    PDEVICE_OBJECT devObj = (PDEVICE_OBJECT)g_DeviceObject;
    PUCHAR devExt = (PUCHAR)devObj->DeviceExtension;
    if (!devExt) { DbgPrintEx(0, 0, "[kbd] devExt NULL\n"); return; }

    DbgPrintEx(0, 0, "[kbd] DevObj =0x%p\n",            devObj);
    DbgPrintEx(0, 0, "[kbd] DevExt =0x%p\n",            devExt);
    DbgPrintEx(0, 0, "[kbd] +0x1C state_flag=0x%08X\n", *(ULONG*)(devExt + 0x1C));
    DbgPrintEx(0, 0, "[kbd] +0x88 devCtx    =0x%p\n",   *(PVOID*)(devExt + 0x88));
    DbgPrintEx(0, 0, "[kbd] +0x90 callback  =0x%p\n",   *(PVOID*)(devExt + 0x90));
}

// ============================================================================
// INJECT KEY
// ============================================================================
NTSTATUS InjectKey(USHORT ScanCode, USHORT Flags) {
    if (!g_DeviceObject) return STATUS_INVALID_PARAMETER;

    PDEVICE_OBJECT devObj = (PDEVICE_OBJECT)g_DeviceObject;
    PUCHAR devExt = (PUCHAR)devObj->DeviceExtension;
    if (!devExt) return STATUS_INVALID_PARAMETER;

    if (*(ULONG*)(devExt + 0x1C) == 0) {
        DbgPrintEx(0, 0, "[kbd] state_flag=0, skip\n");
        return STATUS_UNSUCCESSFUL;
    }

    PVOID                devCtx = *(PVOID*)(devExt + 0x88);
    PKBD_SERVICE_CALLBACK cb    = *(PKBD_SERVICE_CALLBACK*)(devExt + 0x90);

    if (!devCtx || !cb) {
        DbgPrintEx(0, 0, "[kbd] devCtx or cb NULL\n");
        return STATUS_INVALID_PARAMETER;
    }

    PKBD_DATA pkt = (PKBD_DATA)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KBD_DATA), 'KbdI');
    if (!pkt) return STATUS_NO_MEMORY;

    RtlZeroMemory(pkt, sizeof(KBD_DATA));
    pkt->MakeCode = ScanCode;
    pkt->Flags    = Flags;

    DbgPrintEx(0, 0, "[kbd] inject sc=0x%02X flags=0x%04X\n", ScanCode, Flags);

    __try {
        ULONG consumed = 0;
        KIRQL oldIrql = KfRaiseIrql(DISPATCH_LEVEL);
        cb(devCtx, pkt, pkt + 1, &consumed);
        KeLowerIrql(oldIrql);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        DbgPrintEx(0, 0, "[kbd] exception during inject\n");
        ExFreePool(pkt);
        return STATUS_UNSUCCESSFUL;
    }

    ExFreePool(pkt);
    return STATUS_SUCCESS;
}

// ============================================================================
// READ / DELETE / PARSE COMMAND FILE
// ============================================================================
NTSTATUS ReadCommandFile(PCHAR buf, ULONG bufSize, PULONG bytesRead) {
    HANDLE hFile = NULL;
    UNICODE_STRING fn;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    RtlInitUnicodeString(&fn, L"\\??\\C:\\Windows\\Temp\\kbd_cmd.txt");
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
    RtlInitUnicodeString(&fn, L"\\??\\C:\\Windows\\Temp\\kbd_cmd.txt");
    InitializeObjectAttributes(&oa, &fn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    ZwDeleteFile(&oa);
}

// Parse "SCANCODE MODE"  (0x hex or decimal scancode, decimal mode)
NTSTATUS ParseCommand(PCHAR buf, PULONG pScan, PLONG pMode) {
    if (!buf || !buf[0]) return STATUS_INVALID_PARAMETER;
    PCHAR p = buf;

    ULONG scan = 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        while ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
            UCHAR c = *p++;
            scan = scan * 16 + (c >= 'a' ? c-'a'+10 : c >= 'A' ? c-'A'+10 : c-'0');
        }
    } else {
        while (*p >= '0' && *p <= '9') scan = scan * 10 + (*p++ - '0');
    }

    while (*p == ' ' || *p == '\t') p++;
    INT s = (*p == '-') ? (p++, -1) : 1;
    LONG mode = 0;
    while (*p >= '0' && *p <= '9') mode = mode * 10 + (*p++ - '0');
    mode *= s;

    *pScan = scan;
    *pMode = mode;
    return STATUS_SUCCESS;
}

// ============================================================================
// WORKER THREAD
// ============================================================================
VOID WorkerThread(PVOID Context) {
    UNREFERENCED_PARAMETER(Context);

    DbgPrintEx(0, 0, "[kbd] started\n");

    // Step 1: find kbdhid.sys
    FindKbdHidModule();
    if (!g_KbdHidBase) { PsTerminateSystemThread(STATUS_UNSUCCESSFUL); return; }
    DbgPrintEx(0, 0, "[kbd] step 1 ok\n");

    // Step 2: pattern scan for callback
    g_KbdCallback = ScanKbdCallback(g_KbdHidBase, g_KbdHidSize);
    if (!g_KbdCallback) { PsTerminateSystemThread(STATUS_UNSUCCESSFUL); return; }
    DbgPrintEx(0, 0, "[kbd] step 2 ok\n");

    // Step 3: find DeviceObject
    if (!NT_SUCCESS(FindDeviceObject())) { PsTerminateSystemThread(STATUS_UNSUCCESSFUL); return; }
    DbgPrintEx(0, 0, "[kbd] step 3 ok\n");

    // Step 4: inspect DeviceExtension
    InspectDeviceExtension();
    DbgPrintEx(0, 0, "[kbd] ready — C:\\Windows\\Temp\\kbd_cmd.txt\n");

    // Step 5: polling loop (10ms)
    CHAR  cmdBuf[32];
    ULONG bytesRead, scan;
    LONG  mode;
    LARGE_INTEGER delay;
    delay.QuadPart = -100000LL;

    while (TRUE) {
        NTSTATUS st = ReadCommandFile(cmdBuf, sizeof(cmdBuf), &bytesRead);
        if (NT_SUCCESS(st) && bytesRead > 0) {
            if (NT_SUCCESS(ParseCommand(cmdBuf, &scan, &mode))) {
                DeleteCommandFile();

                if (mode == -1) { DbgPrintEx(0, 0, "[kbd] stop\n"); break; }

                USHORT sc = (USHORT)scan;
                if (mode == 0) {
                    InjectKey(sc, 0x0000);
                    KeDelayExecutionThread(KernelMode, FALSE, &delay);
                    InjectKey(sc, 0x0001);
                } else if (mode == 1) {
                    InjectKey(sc, 0x0000);
                } else if (mode == 2) {
                    InjectKey(sc, 0x0001);
                }
            }
        }
        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

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
        DbgPrintEx(0, 0, "[kbd] thread failed: 0x%X\n", status);
        return status;
    }

    ZwClose(hThread);
    return STATUS_SUCCESS;
}
```

## KDMapper Integration

This driver is designed to be **manually mapped into kernel memory via KDMapper** (TheCruZ/kdmapper), NOT loaded via `NtLoadDriver`.

### Entry Point

| Item | Value |
|------|-------|
| **Function name** | `DriverMain` |
| **Signature** | `NTSTATUS DriverMain(PDRIVER_OBJECT, PUNICODE_STRING)` |
| **Linker setting** | Linker → Advanced → Entry Point: `DriverMain` |

### KDMapper Driver Rules Applied

| Rule | Compliance |
|------|-----------|
| **No `IoCreateDevice`** | ✅ All work done via `kbdhid.sys` device objects |
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
  → KDMapper copies kbdhid.sys bytes to kernel (skipping PE header)
  → KDMapper resolves imports and relocations
  → KDMapper calls DriverMain(NULL, NULL)
  → DriverMain spawns WorkerThread
  → WorkerThread:
      1. Finds kbdhid.sys via ZwQuerySystemInformation class 11
      2. Scans for 29-byte callback prologue pattern at VA 0x35C0
      3. Resolves \Driver\kbdhid -> DEVICE_OBJECT
      4. Validates DevExt offsets +0x1C / +0x88 / +0x90
      5. Enters 10ms polling loop for kbd_cmd.txt
```

### See Also

- [KDMapper skill documentation](kdmapper.md) — full rules, CLI parameters, and common errors
- [Mouse injection](mou-injection.md) — mouhid.sys equivalent (mouse version)
- [KDMapper GitHub](https://github.com/TheCruZ/kdmapper) — upstream project

### Mixed alternative

If you need **keyboard and mouse simultaneously** in a single driver, use `hid-inject.sys` instead:

```
D:\skills\dependencies\kdmapper_Release.exe D:\projects\hid-inject\x64\hid-inject.sys
```

It polls both `kbd_cmd.txt` and `mouse_cmd.txt` in the same 10ms loop — both channels are fully independent and can be written concurrently without blocking each other. See `D:\projects\hid-inject\combined\src\main.cpp`.
