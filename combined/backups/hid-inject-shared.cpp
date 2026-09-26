#include <ntifs.h>

// KDMapper variant: mouse commands arrive through a named section and event.
// The entry point stays nonblocking; only the system thread maps the section.
// Do not map this image with kdmapper --free while the thread is running.

enum { MOUSE_QUEUE_CAPACITY = 256, MOUSE_QUEUE_VERSION = 1 };
static const ULONG MOUSE_QUEUE_MAGIC = 0x314D4948; // "HIM1" in little endian

typedef struct _SHARED_MOUSE_COMMAND {
    LONG Mode;
    LONG X;
    LONG Y;
    LONG Reserved;
} SHARED_MOUSE_COMMAND;

typedef struct _SHARED_MOUSE_QUEUE {
    ULONG Magic;                       // published last by the driver
    ULONG Version;
    ULONG Capacity;
    ULONG Reserved;
    volatile LONG WriteIndex;          // single user-mode producer publishes here
    volatile LONG ReadIndex;           // single kernel consumer publishes here
    ULONG Dropped;                     // optional producer-side diagnostic
    ULONG Reserved2;
    SHARED_MOUSE_COMMAND Commands[MOUSE_QUEUE_CAPACITY];
} SHARED_MOUSE_QUEUE;
static_assert(sizeof(SHARED_MOUSE_COMMAND) == 16, "mouse command ABI changed");
static_assert(FIELD_OFFSET(SHARED_MOUSE_QUEUE, Commands) == 32, "mouse queue ABI changed");

static SHARED_MOUSE_QUEUE* g_mouseQueue = NULL;
static HANDLE g_mouseSection = NULL;    // keeps the named section openable
static HANDLE g_mouseWake = NULL;
static ULONG g_mouseCommands = 0;        // consumer thread only
static ULONG g_mouseMovements = 0;       // consumer thread only

// ============================================================================
// GLOBALS — keyboard
// ============================================================================
PVOID g_KbdBase     = NULL;
ULONG g_KbdSize     = 0;
PVOID g_KbdCallback = NULL;
PVOID g_KbdDevice   = NULL;
ULONG g_KbdCtxOff   = 0x88;   // discovered at init
ULONG g_KbdCbOff    = 0x90;

// ============================================================================
// GLOBALS — mouse (callback fallback)
// ============================================================================
PVOID g_MouBase     = NULL;
ULONG g_MouSize     = 0;
PVOID g_MouCallback = NULL;
PVOID g_MouDevice   = NULL;
ULONG g_MouCtxOff   = 0xE0;
ULONG g_MouCbOff    = 0xE8;

// ============================================================================
// GLOBALS — mouclass direct ring buffer injection
// Layout (confirmed from dump): InputData / ReadPtr / WritePtr / InputCount /
//   BufferSizeBytes / SpinLock  — DataEnd is NOT a stored pointer; computed.
// ============================================================================
static PDEVICE_OBJECT g_McClassDev     = NULL;
static ULONG          g_McInputDataOff = 0;  // PMOU_DATA* : ring buffer base
static ULONG          g_McReadPtrOff   = 0;  // PMOU_DATA* : consumer
static ULONG          g_McWritePtrOff  = 0;  // PMOU_DATA* : producer
static ULONG          g_McInputCntOff  = 0;  // ULONG      : packets in buffer
static ULONG          g_McBufSizeOff   = 0;  // ULONG_PTR  : buffer size in bytes
static ULONG          g_McSpinLockOff  = 0;  // KSPIN_LOCK
static ULONG          g_McCurReqOff    = 0;  // PIRP / LIST_ENTRY : pending IRP

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
    USHORT MakeCode;
    USHORT Flags;       // 0x0000=KEY_MAKE  0x0001=KEY_BREAK  0x0002=KEY_E0
    USHORT Reserved;
    ULONG  ExtraInformation;
} KBD_DATA, *PKBD_DATA;

typedef VOID(*PKBD_CALLBACK)(PVOID, PKBD_DATA, PKBD_DATA, PULONG);

// ============================================================================
// MOUSE_INPUT_DATA — 24 bytes
// ============================================================================
#define MOUSE_MOVE_RELATIVE      0x0000
#define MOUSE_MOVE_ABSOLUTE      0x0001
#define MOUSE_LEFT_BUTTON_DOWN   0x0001
#define MOUSE_LEFT_BUTTON_UP     0x0002
#define MOUSE_RIGHT_BUTTON_DOWN  0x0004
#define MOUSE_RIGHT_BUTTON_UP    0x0008
#define MOUSE_MIDDLE_BUTTON_DOWN 0x0010
#define MOUSE_MIDDLE_BUTTON_UP   0x0020

typedef struct {
    USHORT UnitId;
    USHORT Flags;
    USHORT ButtonFlags;
    USHORT ButtonData;
    ULONG  RawButtons;
    LONG   LastX;
    LONG   LastY;
    ULONG  ExtraInformation;
} MOU_DATA, *PMOU_DATA;

typedef VOID(*PMOU_CALLBACK)(PVOID, PMOU_DATA, PMOU_DATA, PULONG);

// ============================================================================
// UTILITIES
// ============================================================================
PCHAR GetFileName(PCHAR p) {
    if (!p) return (PCHAR)"<null>";
    PCHAR last = NULL, cur = p;
    while (*cur) { if (*cur == '\\' || *cur == '/') last = cur; cur++; }
    return last ? last + 1 : p;
}

PVOID PatternScan(PVOID Base, ULONG Size, PUCHAR Pat, ULONG PatSize) {
    if (!Base || Size < PatSize) return NULL;
    PUCHAR p = (PUCHAR)Base;
    for (ULONG i = 0; i <= Size - PatSize; i++) {
        BOOLEAN ok = TRUE;
        for (ULONG j = 0; j < PatSize; j++) { if (p[i+j] != Pat[j]) { ok = FALSE; break; } }
        if (ok) return p + i;
    }
    return NULL;
}

// ============================================================================
// MODULE FINDER
// ============================================================================
VOID FindModule(PCHAR needle, PVOID* outBase, PULONG outSize) {
    ULONG bufSize = 0;
    if (ZwQuerySystemInformation(11, NULL, 0, &bufSize) != STATUS_INFO_LENGTH_MISMATCH) return;

    PSYSTEM_MODULE_INFORMATION mods = (PSYSTEM_MODULE_INFORMATION)
        ExAllocatePool2(POOL_FLAG_NON_PAGED, bufSize, 'HidF');
    if (!mods) return;

    if (!NT_SUCCESS(ZwQuerySystemInformation(11, mods, bufSize, NULL))) {
        ExFreePool(mods); return;
    }

    for (ULONG i = 0; i < mods->ModulesCount; i++) {
        PCHAR name = GetFileName((PCHAR)mods->Modules[i].FullPathName);
        if (strstr(name, needle)) {
            *outBase = mods->Modules[i].ImageBase;
            *outSize = mods->Modules[i].ImageSize;
            break;
        }
    }
    ExFreePool(mods);
}

// ============================================================================
// DEVICE FINDER
// ============================================================================
PVOID FindDevice(PWCHAR driverPath) {
    UNICODE_STRING name;
    RtlInitUnicodeString(&name, driverPath);

    PDRIVER_OBJECT drv = NULL;
    if (!NT_SUCCESS(ObReferenceObjectByName(&name, OBJ_CASE_INSENSITIVE, NULL, 0,
        *IoDriverObjectType, KernelMode, NULL, (PVOID*)&drv)) || !drv)
        return NULL;

    PDEVICE_OBJECT dev = drv->DeviceObject;
    PDEVICE_OBJECT last = NULL;
    int idx = 0;
    while (dev) {
        DbgPrintEx(0, 0, "[hid] [%d] dev=0x%p Type=%d\n", idx++, dev, dev->DeviceType);
        last = dev;
        dev = dev->NextDevice;
    }

    ObDereferenceObject(drv);
    return last; // last = oldest = closest to hardware
}

// Scan device extension for a pointer that falls within [classBase, classBase+classSize).
// Returns the offset of the callback pointer; context is assumed at offset-8.
static BOOLEAN IsKernelPtr(ULONG_PTR v)
{
    // Qualquer endereço canônico do kernel (0xFFFF800000000000+)
    return v >= 0xFFFF800000000000ULL;
}

static BOOLEAN ScanExtForCallback(PUCHAR ext, ULONG extScanSize,
                                   PVOID classBase, ULONG classSize,
                                   PULONG outCbOff, PULONG outCtxOff)
{
    ULONG_PTR lo = (ULONG_PTR)classBase;
    ULONG_PTR hi = lo + classSize;

    // Scan a partir de 0x10 (cobre builds com extension menor)
    for (ULONG off = 0x10; off + 8 <= extScanSize; off += 8) {
        ULONG_PTR candidate = *(ULONG_PTR*)(ext + off);
        if (candidate < lo || candidate >= hi) continue;

        // Tenta contexto em off-8 (layout: ctx, cb) — mais comum
        if (off >= 8) {
            ULONG_PTR ctx = *(ULONG_PTR*)(ext + off - 8);
            if (IsKernelPtr(ctx)) {
                *outCbOff  = off;
                *outCtxOff = off - 8;
                return TRUE;
            }
        }

        // Tenta contexto em off+8 (layout: cb, ctx) — algumas builds
        if (off + 16 <= extScanSize) {
            ULONG_PTR ctx = *(ULONG_PTR*)(ext + off + 8);
            if (IsKernelPtr(ctx)) {
                *outCbOff  = off;
                *outCtxOff = off + 8;
                return TRUE;
            }
        }
    }
    return FALSE;
}

// ============================================================================
// INIT — mouclass direct injection
// Scan the class device extension for the 4-pointer ring buffer cluster:
//   InputData / DataEnd / ReadPointer / WritePointer  (consecutive, 8-byte each)
// Signature: DataEnd-InputData == N*24 (N in 50..500), Read+Write in [InputData,DataEnd).
// ============================================================================
// Real mouclass layout (confirmed from device extension dump):
//   off+0 : InputData       (PMOU_DATA)
//   off+8 : ReadPointer     (PMOU_DATA)
//   off+16: WritePointer    (PMOU_DATA)
//   off+24: InputCount      (ULONG, stored in 8-byte slot with padding)
//   off+32: BufferSizeBytes (ULONG_PTR, e.g. 0x960 = 100*24)
//   off+40: SpinLock        (KSPIN_LOCK)
//   off+48: IRP / LIST_ENTRY (pending read IRP)
// DataEnd is NOT stored — computed as InputData + BufferSizeBytes.
static BOOLEAN ScanMouClassExt(PUCHAR ext, ULONG extSize)
{
    for (ULONG off = 0x10; off + 56 <= extSize; off += 8) {
        ULONG_PTR p0 = *(ULONG_PTR*)(ext + off);       // InputData
        ULONG_PTR p1 = *(ULONG_PTR*)(ext + off +  8);  // ReadPointer
        ULONG_PTR p2 = *(ULONG_PTR*)(ext + off + 16);  // WritePointer

        if (p0 < 0xFFFF800000000000ULL) continue;
        if (p1 < 0xFFFF800000000000ULL) continue;
        if (p2 < 0xFFFF800000000000ULL) continue;

        // ReadPointer and WritePointer must be >= InputData and
        // at a multiple-of-24 offset from it.
        if ((p1 - p0) % sizeof(MOU_DATA) != 0) continue;
        if ((p2 - p0) % sizeof(MOU_DATA) != 0) continue;

        // BufferSizeBytes at off+32: must be N*24 for N in [30, 500]
        ULONG_PTR bufBytes = *(ULONG_PTR*)(ext + off + 32);
        if (bufBytes == 0 || bufBytes % sizeof(MOU_DATA) != 0) continue;
        ULONG n = (ULONG)(bufBytes / sizeof(MOU_DATA));
        if (n < 30 || n > 500) continue;

        // ReadPointer and WritePointer must lie within the buffer
        if (p1 - p0 >= bufBytes) continue;
        if (p2 - p0 >= bufBytes) continue;

        // InputCount at off+24 must be <= n
        ULONG ic = *(ULONG*)(ext + off + 24);
        if (ic > n) continue;

        // SpinLock at off+40 (should be 0 when idle)
        g_McInputDataOff = off;
        g_McReadPtrOff   = off + 8;
        g_McWritePtrOff  = off + 16;
        g_McInputCntOff  = off + 24;
        g_McBufSizeOff   = off + 32;
        g_McSpinLockOff  = off + 40;
        g_McCurReqOff    = off + 48; // LIST_ENTRY head {Flink,Blink} of pending IRPs
        return TRUE;
    }
    return FALSE;
}

static VOID DumpExt64(PUCHAR ext, ULONG size, const char* tag) {
    for (ULONG off = 0; off < size; off += 16) {
        ULONG_PTR a = (off + 0 < size) ? *(ULONG_PTR*)(ext + off)     : 0;
        ULONG_PTR b = (off + 8 < size) ? *(ULONG_PTR*)(ext + off + 8) : 0;
        DbgPrintEx(0, 0, "[hid] %s +%03X: %016llX  %016llX\n", tag, off,
                   (unsigned long long)a, (unsigned long long)b);
    }
}

BOOLEAN InitMouClass() {
    UNICODE_STRING drvName;
    RtlInitUnicodeString(&drvName, L"\\Driver\\mouclass");
    PDRIVER_OBJECT drv = NULL;
    if (!NT_SUCCESS(ObReferenceObjectByName(&drvName, OBJ_CASE_INSENSITIVE, NULL, 0,
            *IoDriverObjectType, KernelMode, NULL, (PVOID*)&drv)) || !drv) {
        DbgPrintEx(0, 0, "[hid] mouclass driver not found\n");
        return FALSE;
    }

    // enumerate ALL mouclass devices (no type filter) and try scan on each
    int idx = 0;
    PDEVICE_OBJECT dev = drv->DeviceObject;
    while (dev) {
        PUCHAR ext = (PUCHAR)dev->DeviceExtension;
        DbgPrintEx(0, 0, "[hid] mouclass[%d] dev=%p Type=%d ExtSize=%lu\n",
                   idx++, dev, dev->DeviceType,
                   dev->DeviceExtension ? 0x200UL : 0UL);

        if (ScanMouClassExt(ext, 0x400)) {
            g_McClassDev = dev;
            DbgPrintEx(0, 0,
                "[hid] mouclass direct OK: id=0x%X rp=0x%X wp=0x%X ic=0x%X bs=0x%X sl=0x%X cr=0x%X\n",
                g_McInputDataOff, g_McReadPtrOff, g_McWritePtrOff,
                g_McInputCntOff,  g_McBufSizeOff, g_McSpinLockOff, g_McCurReqOff);
            break;
        }

        // Dump first 0x200 bytes so we can identify the real layout
        DumpExt64(ext, 0x200, "mc_ext");
        dev = dev->NextDevice;
    }
    ObDereferenceObject(drv);

    if (!g_McClassDev)
        DbgPrintEx(0, 0, "[hid] mouclass direct: scan failed on all devices\n");
    return g_McClassDev != NULL;
}

// ============================================================================
// INJECT — mouse (direct ring buffer, IRP completion, callback fallback)
// ============================================================================
VOID InjectMouse(LONG X, LONG Y, USHORT MoveFlags, USHORT BtnFlags, USHORT BtnData, ULONG RawButtons = 0) {

    // ---- fallback path: callback injection ----
    if (!g_McClassDev) {
        if (!g_MouDevice) return;
        PUCHAR fe = (PUCHAR)((PDEVICE_OBJECT)g_MouDevice)->DeviceExtension;
        PVOID        ctx = *(PVOID*)(fe + g_MouCtxOff);
        PMOU_CALLBACK cb = *(PMOU_CALLBACK*)(fe + g_MouCbOff);
        if (!ctx || !cb) return;
        MOU_DATA pkt; RtlZeroMemory(&pkt, sizeof(pkt));
        pkt.Flags = MoveFlags; pkt.ButtonFlags = BtnFlags;
        pkt.ButtonData = BtnData; pkt.LastX = X; pkt.LastY = Y; pkt.RawButtons = RawButtons;
        __try {
            ULONG c = 0;
            KIRQL irql = KeRaiseIrqlToDpcLevel();
            cb(ctx, &pkt, &pkt + 1, &c);
            KeLowerIrql(irql);
        } __except(EXCEPTION_EXECUTE_HANDLER) {}
        return;
    }

    // ---- direct path: write straight to mouclass ring buffer ----
    PUCHAR ext = (PUCHAR)g_McClassDev->DeviceExtension;
    PKSPIN_LOCK pLock = (PKSPIN_LOCK)(ext + g_McSpinLockOff);
    KIRQL oldIrql = PASSIVE_LEVEL;

    KeAcquireSpinLock(pLock, &oldIrql);

    PMOU_DATA inputData  = *(PMOU_DATA*)(ext + g_McInputDataOff);
    ULONG_PTR bufBytes   = *(ULONG_PTR*)(ext + g_McBufSizeOff);
    PMOU_DATA dataEnd    = inputData + bufBytes / sizeof(MOU_DATA);
    PMOU_DATA writePtr   = *(PMOU_DATA*)(ext + g_McWritePtrOff);
    ULONG     inputCount = *(ULONG*)    (ext + g_McInputCntOff);
    ULONG     capacity   = (ULONG)(bufBytes / sizeof(MOU_DATA));

    if (!inputData || writePtr < inputData || writePtr >= dataEnd) {
        KeReleaseSpinLock(pLock, oldIrql);
        return;
    }

    MOU_DATA pkt;
    RtlZeroMemory(&pkt, sizeof(pkt));
    pkt.Flags       = MoveFlags;
    pkt.ButtonFlags = BtnFlags;
    pkt.ButtonData  = BtnData;
    pkt.LastX       = X;
    pkt.LastY       = Y;
    pkt.RawButtons  = RawButtons;

    *writePtr = pkt;
    writePtr++;
    if (writePtr >= dataEnd) writePtr = inputData;
    *(PMOU_DATA*)(ext + g_McWritePtrOff) = writePtr;
    if (inputCount < capacity) inputCount++;
    *(ULONG*)(ext + g_McInputCntOff) = inputCount;

    // Dequeue first pending IRP from LIST_ENTRY head at g_McCurReqOff.
    // The dump shows: head.Flink == head.Blink == IRP.Tail.Overlay.ListEntry addr.
    // Try candidate offsets for IRP.Tail.Overlay.ListEntry to recover IRP base.
    PIRP pendingIrp = NULL;
    PLIST_ENTRY irpListEntry = NULL;
    {
        PLIST_ENTRY head  = (PLIST_ENTRY)(ext + g_McCurReqOff);
        PLIST_ENTRY flink = head->Flink;
        ULONG_PTR   headAddr = (ULONG_PTR)head;

        if (flink && (ULONG_PTR)flink != headAddr &&
            (ULONG_PTR)flink > 0xFFFF800000000000ULL) {

            // Common offsets for IRP.Tail.Overlay.ListEntry on Windows 10/11 x64
            static const ULONG kOff[] = { 0xD0, 0xC8, 0xB8, 0xA8, 0x98, 0x88, 0x78, 0x68 };
            for (ULONG ki = 0; ki < sizeof(kOff)/sizeof(kOff[0]); ki++) {
                ULONG_PTR base = (ULONG_PTR)flink - kOff[ki];
                if (base < 0xFFFF800000000000ULL) continue;
                __try {
                    if (*(USHORT*)base == 6 /* IO_TYPE_IRP */) {
                        pendingIrp   = (PIRP)base;
                        irpListEntry = flink;
                        break;
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {}
            }

            if (pendingIrp) {
                // Unlink from queue: head → flink.Flink → ... → head
                PLIST_ENTRY next = flink->Flink;
                head->Flink = next;
                next->Blink = head;
            }
        }
    }

    KeReleaseSpinLock(pLock, oldIrql);

    if (!pendingIrp) return; // no IRP found; packet stays buffered

    // Copy buffered packets into the IRP's system buffer and complete it
    PIO_STACK_LOCATION isl = IoGetCurrentIrpStackLocation(pendingIrp);
    PMOU_DATA sysBuf = (PMOU_DATA)pendingIrp->AssociatedIrp.SystemBuffer;
    ULONG maxPkts    = (isl && sysBuf) ? isl->Parameters.Read.Length / sizeof(MOU_DATA) : 0;

    ULONG copied = 0;
    if (maxPkts > 0) {
        KeAcquireSpinLock(pLock, &oldIrql);

        PMOU_DATA readPtr = *(PMOU_DATA*)(ext + g_McReadPtrOff);
        PMOU_DATA id      = *(PMOU_DATA*)(ext + g_McInputDataOff);
        ULONG_PTR bs      = *(ULONG_PTR*)(ext + g_McBufSizeOff);
        PMOU_DATA de      = id + bs / sizeof(MOU_DATA);
        ULONG     ic      = *(ULONG*)    (ext + g_McInputCntOff);

        copied = (ic < maxPkts) ? ic : maxPkts;
        for (ULONG i = 0; i < copied; i++) {
            sysBuf[i] = *readPtr++;
            if (readPtr >= de) readPtr = id;
        }
        ic -= copied;
        *(PMOU_DATA*)(ext + g_McReadPtrOff) = readPtr;
        *(ULONG*)    (ext + g_McInputCntOff) = ic;

        KeReleaseSpinLock(pLock, oldIrql);
    }

    pendingIrp->IoStatus.Status      = STATUS_SUCCESS;
    pendingIrp->IoStatus.Information = copied * sizeof(MOU_DATA);
    IoCompleteRequest(pendingIrp, IO_MOUSE_INCREMENT);
}

// ============================================================================
// INIT — keyboard
// ============================================================================
BOOLEAN InitKeyboard() {
    FindModule((PCHAR)"kbdhid", &g_KbdBase, &g_KbdSize);
    if (!g_KbdBase) { DbgPrintEx(0, 0, "[hid] kbdhid.sys not found\n"); return FALSE; }

    // --- Step 1: find KeyboardClassServiceCallback via pattern scan in kbdhid ---
    // Pattern: prologue of KeyboardClassServiceCallback (stable across builds)
    UCHAR kbdPat[] = {
        0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x48,0x89,0x68,0x10,
        0x48,0x89,0x70,0x20, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56,
        0x41,0x57, 0x48,0x83,0xEC,0x30
    };
    g_KbdCallback = PatternScan(g_KbdBase, g_KbdSize, kbdPat, sizeof(kbdPat));
    if (!g_KbdCallback) { DbgPrintEx(0, 0, "[hid] kbd: pattern not found\n"); return FALSE; }
    DbgPrintEx(0, 0, "[hid] kbd: callback=0x%p (pattern)\n", g_KbdCallback);

    // --- Step 2: find device ---
    g_KbdDevice = FindDevice(L"\\Driver\\kbdhid");
    if (!g_KbdDevice) { DbgPrintEx(0, 0, "[hid] kbdhid device not found\n"); return FALSE; }

    // --- Step 3: find context offset — scan ext for any kernel pointer adjacent
    //             to where the callback pointer points to g_KbdCallback ---
    PVOID kbdclassBase = NULL; ULONG kbdclassSize = 0;
    FindModule((PCHAR)"kbdclass", &kbdclassBase, &kbdclassSize);

    PUCHAR ext = (PUCHAR)((PDEVICE_OBJECT)g_KbdDevice)->DeviceExtension;

    // Prefer: find the ext slot that holds g_KbdCallback, ctx is adjacent
    BOOLEAN found = FALSE;
    for (ULONG off = 0x10; off + 8 <= 0x500 && !found; off += 8) {
        if (*(PVOID*)(ext + off) != g_KbdCallback) continue;
        // try ctx at off-8
        if (off >= 8 && IsKernelPtr(*(ULONG_PTR*)(ext + off - 8))) {
            g_KbdCbOff = off; g_KbdCtxOff = off - 8; found = TRUE;
        }
        // try ctx at off+8
        else if (off + 16 <= 0x500 && IsKernelPtr(*(ULONG_PTR*)(ext + off + 8))) {
            g_KbdCbOff = off; g_KbdCtxOff = off + 8; found = TRUE;
        }
    }

    // Fallback: scan for any pointer within kbdclass range (callback may differ
    // from pattern result if kbdhid stores a trampoline pointer)
    if (!found && kbdclassBase) {
        found = ScanExtForCallback(ext, 0x500, kbdclassBase, kbdclassSize,
                                   &g_KbdCbOff, &g_KbdCtxOff);
        if (found) g_KbdCallback = *(PVOID*)(ext + g_KbdCbOff);
    }

    // Last resort: use pattern result + hardcoded ctx offsets
    if (!found) {
        static const ULONG kbdCtxTry[] = { 0x88, 0x80, 0x90, 0x78, 0x98, 0x70, 0xA0 };
        static const ULONG kbdCbTry[]  = { 0x90, 0x88, 0x98, 0x80, 0xA0, 0x78, 0xA8 };
        for (ULONG i = 0; i < 7 && !found; i++) {
            PVOID ctx = *(PVOID*)(ext + kbdCtxTry[i]);
            PVOID cb  = *(PVOID*)(ext + kbdCbTry[i]);
            if (IsKernelPtr((ULONG_PTR)ctx) && cb) {
                g_KbdCtxOff = kbdCtxTry[i]; g_KbdCbOff = kbdCbTry[i]; found = TRUE;
            }
        }
    }

    if (!found) { DbgPrintEx(0, 0, "[hid] kbd: ctx offset not found (cb=0x%p)\n", g_KbdCallback); return FALSE; }

    DbgPrintEx(0, 0, "[hid] kbd: ctxOff=0x%X cbOff=0x%X cb=0x%p\n",
               g_KbdCtxOff, g_KbdCbOff, g_KbdCallback);
    return TRUE;
}

// ============================================================================
// INIT — mouse
// ============================================================================
BOOLEAN InitMouse() {
    FindModule((PCHAR)"mouhid", &g_MouBase, &g_MouSize);
    if (!g_MouBase) { DbgPrintEx(0, 0, "[hid] mouhid.sys not found\n"); return FALSE; }

    // --- Step 1: find MouseClassServiceCallback via pattern scan in mouhid ---
    UCHAR mouPat[] = {
        0x48,0x89,0x4C,0x24,0x08, 0x55, 0x53, 0x56, 0x57,
        0x41,0x54, 0x41,0x55
    };
    g_MouCallback = PatternScan(g_MouBase, g_MouSize, mouPat, sizeof(mouPat));
    if (!g_MouCallback) { DbgPrintEx(0, 0, "[hid] mou: pattern not found\n"); return FALSE; }
    DbgPrintEx(0, 0, "[hid] mou: callback=0x%p (pattern)\n", g_MouCallback);

    // --- Step 2: find device ---
    g_MouDevice = FindDevice(L"\\Driver\\mouhid");
    if (!g_MouDevice) { DbgPrintEx(0, 0, "[hid] mouhid device not found\n"); return FALSE; }

    // --- Step 3: find context offset ---
    PVOID mouclassBase = NULL; ULONG mouclassSize = 0;
    FindModule((PCHAR)"mouclass", &mouclassBase, &mouclassSize);

    PUCHAR ext = (PUCHAR)((PDEVICE_OBJECT)g_MouDevice)->DeviceExtension;

    // Prefer: find ext slot holding g_MouCallback exactly
    BOOLEAN found = FALSE;
    for (ULONG off = 0x10; off + 8 <= 0x500 && !found; off += 8) {
        if (*(PVOID*)(ext + off) != g_MouCallback) continue;
        if (off >= 8 && IsKernelPtr(*(ULONG_PTR*)(ext + off - 8))) {
            g_MouCbOff = off; g_MouCtxOff = off - 8; found = TRUE;
        } else if (off + 16 <= 0x500 && IsKernelPtr(*(ULONG_PTR*)(ext + off + 8))) {
            g_MouCbOff = off; g_MouCtxOff = off + 8; found = TRUE;
        }
    }

    // Fallback: scan for any mouclass pointer in ext
    if (!found && mouclassBase) {
        found = ScanExtForCallback(ext, 0x500, mouclassBase, mouclassSize,
                                   &g_MouCbOff, &g_MouCtxOff);
        if (found) g_MouCallback = *(PVOID*)(ext + g_MouCbOff);
    }

    // Last resort: hardcoded ctx/cb pairs
    if (!found) {
        static const ULONG mouCtxTry[] = { 0xE0, 0xD8, 0xE8, 0xD0, 0xF0, 0xC8, 0xF8 };
        static const ULONG mouCbTry[]  = { 0xE8, 0xE0, 0xF0, 0xD8, 0xF8, 0xD0, 0x100 };
        for (ULONG i = 0; i < 7 && !found; i++) {
            PVOID ctx = *(PVOID*)(ext + mouCtxTry[i]);
            PVOID cb  = *(PVOID*)(ext + mouCbTry[i]);
            if (IsKernelPtr((ULONG_PTR)ctx) && cb) {
                g_MouCtxOff = mouCtxTry[i]; g_MouCbOff = mouCbTry[i]; found = TRUE;
            }
        }
    }

    if (!found) { DbgPrintEx(0, 0, "[hid] mou: ctx offset not found (cb=0x%p)\n", g_MouCallback); return FALSE; }

    DbgPrintEx(0, 0, "[hid] mou: ctxOff=0x%X cbOff=0x%X cb=0x%p\n",
               g_MouCtxOff, g_MouCbOff, g_MouCallback);
    return TRUE;
}

// ============================================================================
// INJECT — keyboard
// ============================================================================
VOID InjectKey(USHORT ScanCode, USHORT Flags) {
    if (!g_KbdDevice) return;
    PUCHAR ext = (PUCHAR)((PDEVICE_OBJECT)g_KbdDevice)->DeviceExtension;

    PVOID        ctx = *(PVOID*)(ext + g_KbdCtxOff);
    PKBD_CALLBACK cb = *(PKBD_CALLBACK*)(ext + g_KbdCbOff);
    if (!ctx || !cb) return;

    KBD_DATA pkt;
    RtlZeroMemory(&pkt, sizeof(pkt));
    pkt.UnitId   = 0;
    pkt.MakeCode = ScanCode;
    pkt.Flags    = Flags;

    __try {
        ULONG c = 0;
        KIRQL irql = KeRaiseIrqlToDpcLevel();
        cb(ctx, &pkt, &pkt + 1, &c);
        KeLowerIrql(irql);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
}

// InjectMouse is defined above (direct mouclass ring buffer + callback fallback)

// ============================================================================
// DUMP — escreve bytes de uma funcao para arquivo como hex + ascii
// ============================================================================
static const CHAR HEX[] = "0123456789ABCDEF";

static ULONG AppendHex8(PCHAR buf, ULONG pos, UCHAR v) {
    buf[pos++] = HEX[v >> 4];
    buf[pos++] = HEX[v & 0xF];
    return pos;
}
static ULONG AppendHex16(PCHAR buf, ULONG pos, USHORT v) {
    buf[pos++] = HEX[(v >> 12) & 0xF];
    buf[pos++] = HEX[(v >>  8) & 0xF];
    buf[pos++] = HEX[(v >>  4) & 0xF];
    buf[pos++] = HEX[ v        & 0xF];
    return pos;
}

VOID DumpFunc(PVOID func, ULONG n, PCWSTR outPath) {
    if (!func) return;
    PUCHAR p = (PUCHAR)func;

    ULONG lineCount = (n + 7) / 8;
    ULONG bufSize   = lineCount * 50 + 4;
    PCHAR buf = (PCHAR)ExAllocatePool2(POOL_FLAG_NON_PAGED, bufSize, 'DmpF');
    if (!buf) return;

    ULONG pos = 0;
    for (ULONG i = 0; i < n; i += 8) {
        // offset
        pos = AppendHex16(buf, pos, (USHORT)i);
        buf[pos++] = ':'; buf[pos++] = ' ';
        // hex bytes
        for (ULONG j = i; j < i + 8; j++) {
            if (j < n) { pos = AppendHex8(buf, pos, p[j]); buf[pos++] = ' '; }
            else        { buf[pos++]=' '; buf[pos++]=' '; buf[pos++]=' '; }
        }
        buf[pos++] = ' ';
        // ascii
        for (ULONG j = i; j < i + 8 && j < n; j++)
            buf[pos++] = (p[j] >= 0x20 && p[j] < 0x7F) ? (CHAR)p[j] : '.';
        buf[pos++] = '\n';
    }

    UNICODE_STRING fn;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;
    HANDLE h = NULL;
    RtlInitUnicodeString(&fn, outPath);
    InitializeObjectAttributes(&oa, &fn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    if (NT_SUCCESS(ZwCreateFile(&h, GENERIC_WRITE, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, 0, FILE_OVERWRITE_IF,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0))) {
        ZwWriteFile(h, NULL, NULL, NULL, &iosb, buf, pos, NULL, NULL);
        ZwClose(h);
    }
    ExFreePool(buf);
}

// ============================================================================
// COMMAND FILE I/O
// ============================================================================
NTSTATUS ReadFile(PCWSTR path, PCHAR buf, ULONG bufSize, PULONG bytesRead) {
    HANDLE hFile = NULL;
    UNICODE_STRING fn;
    OBJECT_ATTRIBUTES oa;
    IO_STATUS_BLOCK iosb;

    RtlInitUnicodeString(&fn, path);
    InitializeObjectAttributes(&oa, &fn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);

    NTSTATUS st = ZwCreateFile(&hFile, GENERIC_READ, &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL, FILE_SHARE_READ, FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT, NULL, 0);
    if (!NT_SUCCESS(st)) return st;

    st = ZwReadFile(hFile, NULL, NULL, NULL, &iosb, buf, bufSize-1, NULL, NULL);
    if (NT_SUCCESS(st)) { buf[iosb.Information] = '\0'; *bytesRead = (ULONG)iosb.Information; }
    ZwClose(hFile);
    return st;
}

VOID DeleteFile(PCWSTR path) {
    UNICODE_STRING fn;
    OBJECT_ATTRIBUTES oa;
    RtlInitUnicodeString(&fn, path);
    InitializeObjectAttributes(&oa, &fn, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, NULL);
    ZwDeleteFile(&oa);
}

// ============================================================================
// PARSERS
// ============================================================================

// kbd_cmd.txt: "SCANCODE MODE"
//   MODE: 0=press+release  1=press  2=release  -1=stop
VOID ProcessKbd(PCHAR buf) {
    PCHAR p = buf;
    ULONG scan = 0;
    if (p[0]=='0' && (p[1]=='x'||p[1]=='X')) {
        p += 2;
        while ((*p>='0'&&*p<='9')||(*p>='a'&&*p<='f')||(*p>='A'&&*p<='F')) {
            UCHAR c=*p++; scan=scan*16+(c>='a'?c-'a'+10:c>='A'?c-'A'+10:c-'0');
        }
    } else { while (*p>='0'&&*p<='9') scan=scan*10+(*p++-'0'); }

    while (*p==' '||*p=='\t') p++;
    INT s=(*p=='-')?(p++,-1):1;
    LONG mode=0; while (*p>='0'&&*p<='9') mode=mode*10+(*p++-'0'); mode*=s;

    USHORT sc = (USHORT)scan;
    LARGE_INTEGER d; d.QuadPart=-100000LL;

    DbgPrintEx(0, 0, "[hid] kbd sc=0x%02X mode=%ld\n", sc, mode);
    if (mode==0) {
        InjectKey(sc, 0x0000);
        KeDelayExecutionThread(KernelMode, FALSE, &d);
        InjectKey(sc, 0x0001);
    } else if (mode==1) { InjectKey(sc, 0x0000);
    } else if (mode==2) { InjectKey(sc, 0x0001); }
}

// Shared command: MODE, X, Y. -1 stops the worker.
VOID ProcessMou(LONG mode, LONG x, LONG y) {
    LARGE_INTEGER d; d.QuadPart=-800000LL; // 80ms: win32k needs time to re-queue its IRP after completing DOWN

    switch (mode) {
    case  0: InjectMouse(x,y,MOUSE_MOVE_RELATIVE,0,0); break;
    case  1: InjectMouse(x,y,MOUSE_MOVE_ABSOLUTE,0,0); break;
    case  2: DbgPrintEx(0,0,"[hid] mou LClick\n");
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_LEFT_BUTTON_DOWN,0);
             KeDelayExecutionThread(KernelMode,FALSE,&d);
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_LEFT_BUTTON_UP,0); break;
    case  3: DbgPrintEx(0,0,"[hid] mou RClick\n");
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_RIGHT_BUTTON_DOWN,0);
             KeDelayExecutionThread(KernelMode,FALSE,&d);
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_RIGHT_BUTTON_UP,0); break;
    case  4: DbgPrintEx(0,0,"[hid] mou LDown\n");
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_LEFT_BUTTON_DOWN,0); break;
    case  5: DbgPrintEx(0,0,"[hid] mou LUp\n");
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_LEFT_BUTTON_UP,0); break;
    case  6: DbgPrintEx(0,0,"[hid] mou RDown\n");
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_RIGHT_BUTTON_DOWN,0); break;
    case  7: DbgPrintEx(0,0,"[hid] mou RUp\n");
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_RIGHT_BUTTON_UP,0); break;

    // Style B: RawButtons tracking (bit 0 = LMB cumulative state)
    case  9: DbgPrintEx(0,0,"[hid] mou LDown+RawBtn\n");
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_LEFT_BUTTON_DOWN,0,1); break;
    case 10: DbgPrintEx(0,0,"[hid] mou LUp+RawBtn\n");
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_LEFT_BUTTON_UP,0,0); break;

    // Style C: micro relative movement combined with DOWN (dx=-1 or +1)
    case 11: DbgPrintEx(0,0,"[hid] mou LDown+Move dx=%ld\n",x);
             InjectMouse(x,0,MOUSE_MOVE_RELATIVE,MOUSE_LEFT_BUTTON_DOWN,0,0); break;
    case 12: DbgPrintEx(0,0,"[hid] mou LUp\n");
             InjectMouse(0,0,MOUSE_MOVE_RELATIVE,MOUSE_LEFT_BUTTON_UP,0,0); break;

    default: DbgPrintEx(0,0,"[hid] mou unknown %ld\n",mode); break;
    }
}

// ============================================================================
// SHARED MOUSE IPC
// Kernel names below correspond to Global\HidInjectMouseQueueV1 and
// Global\HidInjectMouseWakeV1 in Win32. Only an elevated Administrators
// process or LocalSystem can open them. The section has exactly one producer.
// ============================================================================
static BOOLEAN InitSharedMouse() {
    SECURITY_DESCRIPTOR sd;
    UCHAR aclBytes[128] = {};
    PACL acl = (PACL)aclBytes;
    NTSTATUS st = RtlCreateAcl(acl, sizeof(aclBytes), ACL_REVISION);
    if (!NT_SUCCESS(st)) return FALSE;

    const ACCESS_MASK sharedAccess = SECTION_ALL_ACCESS | EVENT_ALL_ACCESS;
    st = RtlAddAccessAllowedAce(acl, ACL_REVISION, sharedAccess,
                                SeExports->SeLocalSystemSid);
    if (!NT_SUCCESS(st)) return FALSE;
    st = RtlAddAccessAllowedAce(acl, ACL_REVISION, sharedAccess,
                                SeExports->SeAliasAdminsSid);
    if (!NT_SUCCESS(st)) return FALSE;
    st = RtlCreateSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION);
    if (!NT_SUCCESS(st)) return FALSE;
    st = RtlSetDaclSecurityDescriptor(&sd, TRUE, acl, FALSE);
    if (!NT_SUCCESS(st)) return FALSE;

    UNICODE_STRING name;
    OBJECT_ATTRIBUTES oa;
    LARGE_INTEGER sectionSize;
    sectionSize.QuadPart = sizeof(SHARED_MOUSE_QUEUE);
    RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\HidInjectMouseQueueV1");
    InitializeObjectAttributes(&oa, &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, &sd);
    st = ZwCreateSection(&g_mouseSection, SECTION_ALL_ACCESS, &oa, &sectionSize,
                         PAGE_READWRITE, SEC_COMMIT, NULL);
    if (!NT_SUCCESS(st)) {
        DbgPrintEx(0, 0, "[hid] shared section create failed: 0x%X\n", st);
        return FALSE;
    }

    PVOID view = NULL;
    SIZE_T viewSize = sizeof(SHARED_MOUSE_QUEUE);
    st = ZwMapViewOfSection(g_mouseSection, ZwCurrentProcess(), &view, 0, 0, NULL,
                            &viewSize, ViewUnmap, 0, PAGE_READWRITE);
    if (!NT_SUCCESS(st)) {
        DbgPrintEx(0, 0, "[hid] shared section map failed: 0x%X\n", st);
        ZwClose(g_mouseSection);
        g_mouseSection = NULL;
        return FALSE;
    }

    g_mouseQueue = (SHARED_MOUSE_QUEUE*)view;
    RtlZeroMemory(g_mouseQueue, sizeof(*g_mouseQueue));
    g_mouseQueue->Version = MOUSE_QUEUE_VERSION;
    g_mouseQueue->Capacity = MOUSE_QUEUE_CAPACITY;

    RtlInitUnicodeString(&name, L"\\BaseNamedObjects\\HidInjectMouseWakeV1");
    InitializeObjectAttributes(&oa, &name,
        OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, NULL, &sd);
    st = ZwCreateEvent(&g_mouseWake, EVENT_ALL_ACCESS, &oa,
                       SynchronizationEvent, FALSE);
    if (!NT_SUCCESS(st)) {
        DbgPrintEx(0, 0, "[hid] shared event create failed: 0x%X\n", st);
        ZwUnmapViewOfSection(ZwCurrentProcess(), g_mouseQueue);
        g_mouseQueue = NULL;
        ZwClose(g_mouseSection);
        g_mouseSection = NULL;
        return FALSE;
    }

    // Interlocked store publishes initialized metadata to the producer.
    InterlockedExchange((volatile LONG*)&g_mouseQueue->Magic,
                        (LONG)MOUSE_QUEUE_MAGIC);
    DbgPrintEx(0, 0, "[hid][shm] ready: queue=Global\\HidInjectMouseQueueV1 "
                     "wake=Global\\HidInjectMouseWakeV1 capacity=%lu\n",
               (ULONG)MOUSE_QUEUE_CAPACITY);
    return TRUE;
}

static VOID CloseSharedMouse() {
    if (g_mouseQueue) {
        DbgPrintEx(0, 0, "[hid][shm] closing: commands=%lu movements=%lu dropped=%lu\n",
                   g_mouseCommands, g_mouseMovements, g_mouseQueue->Dropped);
        InterlockedExchange((volatile LONG*)&g_mouseQueue->Magic, 0);
        ZwUnmapViewOfSection(ZwCurrentProcess(), g_mouseQueue);
        g_mouseQueue = NULL;
    }
    if (g_mouseWake) {
        ZwClose(g_mouseWake);
        g_mouseWake = NULL;
    }
    if (g_mouseSection) {
        ZwClose(g_mouseSection);
        g_mouseSection = NULL;
    }
}

// Returns FALSE for the -1 stop command or a corrupt queue index.
static BOOLEAN DrainSharedMouse() {
    for (ULONG i = 0; i < MOUSE_QUEUE_CAPACITY; ++i) {
        const LONG read = InterlockedCompareExchange(&g_mouseQueue->ReadIndex, 0, 0);
        const LONG write = InterlockedCompareExchange(&g_mouseQueue->WriteIndex, 0, 0);
        const ULONG available = (ULONG)write - (ULONG)read;
        if (available == 0) return TRUE;
        if (available > MOUSE_QUEUE_CAPACITY) {
            DbgPrintEx(0, 0, "[hid] corrupt shared queue indices\n");
            return FALSE;
        }

        // Copy from the pageable shared view before InjectMouse may raise IRQL.
        SHARED_MOUSE_COMMAND cmd = g_mouseQueue->Commands[(ULONG)read % MOUSE_QUEUE_CAPACITY];
        InterlockedExchange(&g_mouseQueue->ReadIndex, (LONG)((ULONG)read + 1));
        ++g_mouseCommands;
        if (cmd.Mode == 0 || cmd.Mode == 1) ++g_mouseMovements;
        // The first commands confirm end-to-end IPC in DebugView. Thereafter,
        // emit one sample per 1024 commands to avoid slowing mouse movement.
        if (g_mouseCommands <= 8 || (g_mouseCommands & 1023) == 0) {
            DbgPrintEx(0, 0, "[hid][shm] cmd #%lu mode=%ld x=%ld y=%ld "
                             "pending=%lu dropped=%lu\n",
                       g_mouseCommands, cmd.Mode, cmd.X, cmd.Y,
                       available - 1, g_mouseQueue->Dropped);
        }
        if (cmd.Mode == -1) return FALSE;
        if (cmd.Mode >= 0 && cmd.Mode <= 12 && cmd.Mode != 8)
            ProcessMou(cmd.Mode, cmd.X, cmd.Y);
    }
    return TRUE;
}

// ============================================================================
// WORKER THREAD
// Keyboard retains its 10 ms file polling. Mouse uses event-driven shared IPC.
// ============================================================================
VOID WorkerThread(PVOID Context) {
    UNREFERENCED_PARAMETER(Context);
    DbgPrintEx(0, 0, "[hid] started\n");

    // --- keyboard init ---
    if (!InitKeyboard()) { PsTerminateSystemThread(STATUS_UNSUCCESSFUL); return; }
    DbgPrintEx(0, 0, "[hid] kbd ok\n");

    // --- mouse: try direct mouclass ring buffer first, fall back to callback ---
    InitMouse(); // populates fallback globals (non-fatal if fails)
    if (!InitMouClass() && !g_MouDevice) {
        DbgPrintEx(0, 0, "[hid] mou init failed entirely\n");
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
        return;
    }
    if (!InitSharedMouse()) {
        PsTerminateSystemThread(STATUS_UNSUCCESSFUL);
        return;
    }
    DbgPrintEx(0, 0, "[hid] mou ok (direct=%d fallback=%d)\n",
               g_McClassDev != NULL, g_MouDevice != NULL);

    DbgPrintEx(0, 0, "[hid] ready\n");
    DbgPrintEx(0, 0, "[hid]   kbd -> C:\\Windows\\Temp\\kbd_cmd.txt\n");
    DbgPrintEx(0, 0, "[hid]   mou -> Global\\HidInjectMouseQueueV1\n");

    CHAR  buf[32];
    ULONG br;
    LARGE_INTEGER delay; delay.QuadPart = -100000LL; // 10ms

    PCWSTR kbdPath = L"\\??\\C:\\Windows\\Temp\\kbd_cmd.txt";
    while (TRUE) {
        // --- check keyboard ---
        if (NT_SUCCESS(ReadFile(kbdPath, buf, sizeof(buf), &br)) && br > 0) {
            // check for stop before processing
            PCHAR p = buf; while (*p==' '||*p=='\t') p++;
            if (p[0]=='-' && p[1]=='1') {
                DeleteFile(kbdPath);
                DbgPrintEx(0, 0, "[hid] stop\n");
                break;
            }
            DeleteFile(kbdPath);
            ProcessKbd(buf);
        }

        if (!DrainSharedMouse()) {
            DbgPrintEx(0, 0, "[hid] mouse queue stopped\n");
            break;
        }
        // A signaled auto-reset event wakes the worker immediately. The timeout
        // preserves the keyboard's existing 10 ms polling cadence.
        if (InterlockedCompareExchange(&g_mouseQueue->ReadIndex, 0, 0) ==
            InterlockedCompareExchange(&g_mouseQueue->WriteIndex, 0, 0)) {
            NTSTATUS waitStatus = ZwWaitForSingleObject(g_mouseWake, FALSE, &delay);
            if (waitStatus != STATUS_SUCCESS && waitStatus != STATUS_TIMEOUT) {
                DbgPrintEx(0, 0, "[hid][shm] event wait failed: 0x%X\n", waitStatus);
                break;
            }
        }
    }

    CloseSharedMouse();
    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ============================================================================
// ENTRY POINT
// ============================================================================
// Named event visible to userspace: Global\HidInjectLoaded
// Loader checks this before mapping — if it exists, driver is already live.
static HANDLE g_aliveEvent = NULL;

extern "C" NTSTATUS DriverMain(
    _In_ PDRIVER_OBJECT  kdmapperParam1,
    _In_ PUNICODE_STRING kdmapperParam2)
{
    UNREFERENCED_PARAMETER(kdmapperParam1);
    UNREFERENCED_PARAMETER(kdmapperParam2);

    // Create a persistent named event so userspace can detect us via OpenEvent("Global\\HidInjectLoaded")
    UNICODE_STRING evName;
    RtlInitUnicodeString(&evName, L"\\BaseNamedObjects\\Global\\HidInjectLoaded");
    OBJECT_ATTRIBUTES evOa;
    InitializeObjectAttributes(&evOa, &evName, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE, NULL, NULL);
    ZwCreateEvent(&g_aliveEvent, EVENT_ALL_ACCESS, &evOa, NotificationEvent, TRUE);

    HANDLE hThread = NULL;
    NTSTATUS st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS,
        NULL, NULL, NULL, WorkerThread, NULL);

    if (!NT_SUCCESS(st)) { DbgPrintEx(0, 0, "[hid] thread failed: 0x%X\n", st); return st; }
    ZwClose(hThread);
    return STATUS_SUCCESS;
}
