#include <ntddk.h>

// ============================================================================
// GLOBALS — keyboard
// ============================================================================
PVOID g_KbdBase     = NULL;
ULONG g_KbdSize     = 0;
PVOID g_KbdCallback = NULL;
PVOID g_KbdDevice   = NULL;

// ============================================================================
// GLOBALS — mouse
// ============================================================================
PVOID g_MouBase     = NULL;
ULONG g_MouSize     = 0;
PVOID g_MouCallback = NULL;
PVOID g_MouDevice   = NULL;

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

// ============================================================================
// INIT — keyboard
// ============================================================================
BOOLEAN InitKeyboard() {
    FindModule((PCHAR)"kbdhid", &g_KbdBase, &g_KbdSize);
    if (!g_KbdBase) { DbgPrintEx(0, 0, "[hid] kbdhid.sys not found\n"); return FALSE; }
    DbgPrintEx(0, 0, "[hid] kbdhid base=0x%p size=0x%X\n", g_KbdBase, g_KbdSize);

    UCHAR kbdPat[] = {
        0x48,0x8B,0xC4, 0x48,0x89,0x58,0x08, 0x48,0x89,0x68,0x10,
        0x48,0x89,0x70,0x20, 0x57, 0x41,0x54, 0x41,0x55, 0x41,0x56,
        0x41,0x57, 0x48,0x83,0xEC,0x30
    };
    g_KbdCallback = PatternScan(g_KbdBase, g_KbdSize, kbdPat, sizeof(kbdPat));
    if (!g_KbdCallback) { DbgPrintEx(0, 0, "[hid] kbd callback not found\n"); return FALSE; }
    DbgPrintEx(0, 0, "[hid] kbd callback=0x%p\n", g_KbdCallback);

    g_KbdDevice = FindDevice(L"\\Driver\\kbdhid");
    if (!g_KbdDevice) { DbgPrintEx(0, 0, "[hid] kbdhid device not found\n"); return FALSE; }
    DbgPrintEx(0, 0, "[hid] kbd device=0x%p\n", g_KbdDevice);

    PUCHAR ext = (PUCHAR)((PDEVICE_OBJECT)g_KbdDevice)->DeviceExtension;
    DbgPrintEx(0, 0, "[hid] kbd +0x1C=0x%08X +0x88=0x%p +0x90=0x%p\n",
        *(ULONG*)(ext+0x1C), *(PVOID*)(ext+0x88), *(PVOID*)(ext+0x90));
    return TRUE;
}

// ============================================================================
// INIT — mouse
// ============================================================================
BOOLEAN InitMouse() {
    FindModule((PCHAR)"mouhid", &g_MouBase, &g_MouSize);
    if (!g_MouBase) { DbgPrintEx(0, 0, "[hid] mouhid.sys not found\n"); return FALSE; }
    DbgPrintEx(0, 0, "[hid] mouhid base=0x%p size=0x%X\n", g_MouBase, g_MouSize);

    UCHAR mouPat[] = {
        0x48,0x89,0x4C,0x24,0x08, 0x55, 0x53, 0x56, 0x57,
        0x41,0x54, 0x41,0x55
    };
    g_MouCallback = PatternScan(g_MouBase, g_MouSize, mouPat, sizeof(mouPat));
    if (!g_MouCallback) { DbgPrintEx(0, 0, "[hid] mou callback not found\n"); return FALSE; }
    DbgPrintEx(0, 0, "[hid] mou callback=0x%p\n", g_MouCallback);

    // log all mouhid devices with mou-specific offsets
    {
        UNICODE_STRING n2; RtlInitUnicodeString(&n2, L"\\Driver\\mouhid");
        PDRIVER_OBJECT drv2 = NULL;
        if (NT_SUCCESS(ObReferenceObjectByName(&n2, OBJ_CASE_INSENSITIVE, NULL, 0,
            *IoDriverObjectType, KernelMode, NULL, (PVOID*)&drv2)) && drv2) {
            PDEVICE_OBJECT d = drv2->DeviceObject; int i = 0;
            while (d) {
                PUCHAR e = (PUCHAR)d->DeviceExtension;
                DbgPrintEx(0, 0, "[hid] mou[%d] dev=0x%p +0x1C=%08X +0xE0=0x%p +0xE8=0x%p\n",
                    i++, d, *(ULONG*)(e+0x1C), *(PVOID*)(e+0xE0), *(PVOID*)(e+0xE8));
                d = d->NextDevice;
            }
            ObDereferenceObject(drv2);
        }
    }

    g_MouDevice = FindDevice(L"\\Driver\\mouhid");
    if (!g_MouDevice) { DbgPrintEx(0, 0, "[hid] mouhid device not found\n"); return FALSE; }
    DbgPrintEx(0, 0, "[hid] mou selected=0x%p\n", g_MouDevice);

    PUCHAR ext = (PUCHAR)((PDEVICE_OBJECT)g_MouDevice)->DeviceExtension;
    DbgPrintEx(0, 0, "[hid] mou +0x1C=0x%08X +0xE0=0x%p +0xE8=0x%p\n",
        *(ULONG*)(ext+0x1C), *(PVOID*)(ext+0xE0), *(PVOID*)(ext+0xE8));
    return TRUE;
}

// ============================================================================
// INJECT — keyboard
// ============================================================================
VOID InjectKey(USHORT ScanCode, USHORT Flags) {
    if (!g_KbdCallback || !g_KbdDevice) return;
    PUCHAR ext = (PUCHAR)((PDEVICE_OBJECT)g_KbdDevice)->DeviceExtension;
    if (*(ULONG*)(ext+0x1C) == 0) {
        DbgPrintEx(0, 0, "[hid] kbd state_flag=0, forcing 1\n");
        *(ULONG*)(ext+0x1C) = 1;
    }

    PKBD_DATA pkt = (PKBD_DATA)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(KBD_DATA), 'KbdI');
    if (!pkt) return;
    RtlZeroMemory(pkt, sizeof(KBD_DATA));
    pkt->MakeCode = ScanCode;
    pkt->Flags    = Flags;

    PUCHAR ext2 = (PUCHAR)((PDEVICE_OBJECT)g_KbdDevice)->DeviceExtension;
    PVOID        ctx = *(PVOID*)(ext2+0x88);
    PKBD_CALLBACK cb = *(PKBD_CALLBACK*)(ext2+0x90);
    if (!ctx || !cb) { ExFreePool(pkt); return; }

    __try {
        ULONG c = 0;
        KIRQL irql = KfRaiseIrql(DISPATCH_LEVEL);
        cb(ctx, pkt, pkt+1, &c);
        KeLowerIrql(irql);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    ExFreePool(pkt);
}

// ============================================================================
// INJECT — mouse
// ============================================================================
VOID InjectMouse(LONG X, LONG Y, USHORT MoveFlags, USHORT BtnFlags, USHORT BtnData) {
    if (!g_MouCallback || !g_MouDevice) return;
    PUCHAR ext = (PUCHAR)((PDEVICE_OBJECT)g_MouDevice)->DeviceExtension;
    if (*(ULONG*)(ext+0x1C) == 0) {
        DbgPrintEx(0, 0, "[hid] mou state_flag=0, forcing 1\n");
        *(ULONG*)(ext+0x1C) = 1;
    }

    PMOU_DATA pkt = (PMOU_DATA)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(MOU_DATA), 'MouI');
    if (!pkt) return;
    RtlZeroMemory(pkt, sizeof(MOU_DATA));
    pkt->Flags       = MoveFlags;
    pkt->ButtonFlags = BtnFlags;
    pkt->ButtonData  = BtnData;
    pkt->LastX       = X;
    pkt->LastY       = Y;

    PUCHAR ext2 = (PUCHAR)((PDEVICE_OBJECT)g_MouDevice)->DeviceExtension;
    PVOID        ctx = *(PVOID*)(ext2+0xE0);
    PMOU_CALLBACK cb = *(PMOU_CALLBACK*)(ext2+0xE8);
    if (!ctx || !cb) { ExFreePool(pkt); return; }

    __try {
        ULONG c = 0;
        KIRQL irql = KfRaiseIrql(DISPATCH_LEVEL);
        cb(ctx, pkt, pkt+1, &c);
        KeLowerIrql(irql);
    } __except(EXCEPTION_EXECUTE_HANDLER) {}

    ExFreePool(pkt);
}

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

// mouse_cmd.txt: "MODE X Y"
//   0=rel  1=abs  2=lclick  3=rclick  4=ld  5=lu  6=rd  7=ru  -1=stop
VOID ProcessMou(PCHAR buf) {
    PCHAR p = buf;
    INT s=(*p=='-')?(p++,-1):1;
    LONG mode=0; while (*p>='0'&&*p<='9') mode=mode*10+(*p++-'0'); mode*=s;
    while (*p==' '||*p=='\t') p++;
    s=(*p=='-')?(p++,-1):1; LONG x=0; while (*p>='0'&&*p<='9') x=x*10+(*p++-'0'); x*=s;
    while (*p==' '||*p=='\t') p++;
    s=(*p=='-')?(p++,-1):1; LONG y=0; while (*p>='0'&&*p<='9') y=y*10+(*p++-'0'); y*=s;

    LARGE_INTEGER d; d.QuadPart=-100000LL;

    switch (mode) {
    case  0: DbgPrintEx(0,0,"[hid] mou rel X=%ld Y=%ld\n",x,y);
             InjectMouse(x,y,MOUSE_MOVE_RELATIVE,0,0); break;
    case  1: DbgPrintEx(0,0,"[hid] mou abs X=%ld Y=%ld\n",x,y);
             InjectMouse(x,y,MOUSE_MOVE_ABSOLUTE,0,0); break;
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
    default: DbgPrintEx(0,0,"[hid] mou unknown %ld\n",mode); break;
    }
}

// ============================================================================
// WORKER THREAD
// Polls both command files simultaneously every 10ms.
// kbd_cmd.txt  and  mouse_cmd.txt are independent — both processed each tick.
// -1 in either file stops the thread.
// ============================================================================
VOID WorkerThread(PVOID Context) {
    UNREFERENCED_PARAMETER(Context);
    DbgPrintEx(0, 0, "[hid] started\n");

    // --- keyboard init ---
    if (!InitKeyboard()) { PsTerminateSystemThread(STATUS_UNSUCCESSFUL); return; }
    DbgPrintEx(0, 0, "[hid] kbd ok\n");

    // --- mouse init ---
    if (!InitMouse()) { PsTerminateSystemThread(STATUS_UNSUCCESSFUL); return; }
    DbgPrintEx(0, 0, "[hid] mou ok\n");

    // dump callbacks internos (HID)
    DumpFunc(g_KbdCallback, 64, L"\\??\\C:\\Windows\\Temp\\kbd_hid_dump.txt");
    DumpFunc(g_MouCallback, 64, L"\\??\\C:\\Windows\\Temp\\mou_hid_dump.txt");
    // dump callbacks DevExt (class) — o que realmente chamamos para injetar
    {
        PUCHAR ke = (PUCHAR)((PDEVICE_OBJECT)g_KbdDevice)->DeviceExtension;
        PUCHAR me = (PUCHAR)((PDEVICE_OBJECT)g_MouDevice)->DeviceExtension;
        DumpFunc(*(PVOID*)(ke+0x90), 32, L"\\??\\C:\\Windows\\Temp\\kbd_cls_dump.txt");
        DumpFunc(*(PVOID*)(me+0xE8), 32, L"\\??\\C:\\Windows\\Temp\\mou_cls_dump.txt");
    }
    DbgPrintEx(0, 0, "[hid] dumps escritos em C:\\Windows\\Temp\\*_dump.txt\n");

    DbgPrintEx(0, 0, "[hid] ready\n");
    DbgPrintEx(0, 0, "[hid]   kbd -> C:\\Windows\\Temp\\kbd_cmd.txt\n");
    DbgPrintEx(0, 0, "[hid]   mou -> C:\\Windows\\Temp\\mouse_cmd.txt\n");

    CHAR  buf[32];
    ULONG br;
    LARGE_INTEGER delay; delay.QuadPart = -100000LL; // 10ms

    PCWSTR kbdPath = L"\\??\\C:\\Windows\\Temp\\kbd_cmd.txt";
    PCWSTR mouPath = L"\\??\\C:\\Windows\\Temp\\mouse_cmd.txt";

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

        // --- check mouse ---
        if (NT_SUCCESS(ReadFile(mouPath, buf, sizeof(buf), &br)) && br > 0) {
            PCHAR p = buf; while (*p==' '||*p=='\t') p++;
            if (p[0]=='-' && p[1]=='1') {
                DeleteFile(mouPath);
                DbgPrintEx(0, 0, "[hid] stop\n");
                break;
            }
            DeleteFile(mouPath);
            ProcessMou(buf);
        }

        KeDelayExecutionThread(KernelMode, FALSE, &delay);
    }

    PsTerminateSystemThread(STATUS_SUCCESS);
}

// ============================================================================
// ENTRY POINT
// ============================================================================
extern "C" NTSTATUS DriverMain(
    _In_ PDRIVER_OBJECT  kdmapperParam1,
    _In_ PUNICODE_STRING kdmapperParam2)
{
    UNREFERENCED_PARAMETER(kdmapperParam1);
    UNREFERENCED_PARAMETER(kdmapperParam2);

    HANDLE hThread = NULL;
    NTSTATUS st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS,
        NULL, NULL, NULL, WorkerThread, NULL);

    if (!NT_SUCCESS(st)) { DbgPrintEx(0, 0, "[hid] thread failed: 0x%X\n", st); return st; }
    ZwClose(hThread);
    return STATUS_SUCCESS;
}
