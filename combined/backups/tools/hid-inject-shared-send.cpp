#include <windows.h>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdio>
#include <cwchar>

// Matches both shared variants. Mode 20 sends a keyboard scan code in X and
// an action in Y (0=tap, 1=down, 2=up). Keep this process alive for high-rate input:
// launching a new process for every mouse movement defeats the IPC improvement.
static const ULONG kMagic = 0x314D4948;
static const ULONG kCapacity = 256;

struct MouseCommand {
    LONG Mode;
    LONG X;
    LONG Y;
    LONG Reserved;
};

struct MouseQueue {
    ULONG Magic;
    ULONG Version;
    ULONG Capacity;
    ULONG Reserved;
    volatile LONG WriteIndex;
    volatile LONG ReadIndex;
    volatile LONG Dropped;
    ULONG Reserved2;
    MouseCommand Commands[kCapacity];
};

static_assert(sizeof(MouseCommand) == 16, "mouse command ABI changed");
static_assert(offsetof(MouseQueue, Commands) == 32, "mouse queue ABI changed");

static bool ParseLong(const wchar_t* text, LONG* value) {
    if (!text || !*text) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const long parsed = wcstol(text, &end, 10);
    if (errno == ERANGE || end == text || *end != L'\0' ||
        parsed < LONG_MIN || parsed > LONG_MAX) return false;
    *value = static_cast<LONG>(parsed);
    return true;
}

static bool Send(MouseQueue* queue, HANDLE wake, LONG mode, LONG x, LONG y) {
    const ULONGLONG deadline = GetTickCount64() + 100;
    for (;;) {
        if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&queue->Magic), 0, 0)
            != static_cast<LONG>(kMagic)) return false;

        const LONG write = InterlockedCompareExchange(&queue->WriteIndex, 0, 0);
        const LONG read = InterlockedCompareExchange(&queue->ReadIndex, 0, 0);
        if (static_cast<ULONG>(write) - static_cast<ULONG>(read) < kCapacity) {
            queue->Commands[static_cast<ULONG>(write) % kCapacity] = {mode, x, y, 0};
            InterlockedExchange(&queue->WriteIndex,
                                static_cast<LONG>(static_cast<ULONG>(write) + 1));
            return SetEvent(wake) != 0;
        }
        if (GetTickCount64() >= deadline) {
            InterlockedIncrement(&queue->Dropped);
            return false;
        }
        Sleep(1);
    }
}

int wmain(int argc, wchar_t** argv) {
    const bool stream = argc == 2 && wcscmp(argv[1], L"--stdin") == 0;
    if (!stream && argc != 4) {
        fwprintf(stderr, L"Usage: hid-inject-shared-send MODE X Y\n"
                         L"   or: hid-inject-shared-send --stdin  (one MODE X Y per line)\n"
                         L"Keyboard: MODE=20, X=scan code, Y=0 tap / 1 down / 2 up\n");
        return 2;
    }

    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE,
                                       L"Global\\HidInjectMouseQueueV1");
    if (!mapping) {
        fwprintf(stderr, L"OpenFileMapping failed: %lu (run elevated after driver starts)\n",
                 GetLastError());
        return 1;
    }
    HANDLE wake = OpenEventW(EVENT_MODIFY_STATE, FALSE,
                             L"Global\\HidInjectMouseWakeV1");
    if (!wake) {
        fwprintf(stderr, L"OpenEvent failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        return 1;
    }
    MouseQueue* queue = static_cast<MouseQueue*>(
        MapViewOfFile(mapping, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, sizeof(MouseQueue)));
    if (!queue || queue->Magic != kMagic || queue->Version != 1 ||
        queue->Capacity != kCapacity) {
        fwprintf(stderr, L"Mouse queue is unavailable or has an incompatible layout.\n");
        if (queue) UnmapViewOfFile(queue);
        CloseHandle(wake);
        CloseHandle(mapping);
        return 1;
    }

    int result = 0;
    if (stream) {
        wchar_t line[128];
        while (fgetws(line, static_cast<int>(sizeof(line) / sizeof(line[0])), stdin)) {
            LONG mode, x, y;
            wchar_t extra;
            if (swscanf_s(line, L"%ld %ld %ld %lc", &mode, &x, &y, &extra, 1) != 3) {
                fwprintf(stderr, L"Invalid command: %ls", line);
                result = 2;
                break;
            }
            if (!Send(queue, wake, mode, x, y)) {
                fwprintf(stderr, L"Mouse queue full, stopped, or event unavailable.\n");
                result = 1;
                break;
            }
        }
    } else {
        LONG mode, x, y;
        if (!ParseLong(argv[1], &mode) || !ParseLong(argv[2], &x) ||
            !ParseLong(argv[3], &y)) {
            fwprintf(stderr, L"MODE, X and Y must be signed 32-bit integers.\n");
            result = 2;
        } else if (!Send(queue, wake, mode, x, y)) {
            fwprintf(stderr, L"Mouse queue full, stopped, or event unavailable.\n");
            result = 1;
        }
    }

    UnmapViewOfFile(queue);
    CloseHandle(wake);
    CloseHandle(mapping);
    return result;
}
