#include <windows.h>
#include <grimace/extras.h>
#include <pkfuncs.h>
#include <stdint.h>
#include <string.h>

#include "present.h"
#include "timebase.h"
#include "log.h"

void CacheRangeFlush(LPVOID address, DWORD length, DWORD flags);

#define CACHE_SYNC_WRITEBACK 0x004
#define PAGE_BYTES 4096
#define PRESENT_BLOCK_PIXELS 8
#define PAGE_ATTRIBUTES_WRITE_BACK 0x0000000C
#define PAGE_ATTRIBUTES_CACHE_MASK 0x0000000C
#define HANDLE_TYPE_PROCESS 2
#define PROCESS_SET_ATTRIBUTES 3
#define SET_ATTRIBUTES_ARGUMENTS 5

typedef BOOL (*process_set_attributes_fn)(DWORD handle_type, DWORD method, DWORD error,
                                          DWORD arguments, HANDLE process, LPVOID address,
                                          DWORD bytes, DWORD new_flags, DWORD mask,
                                          LPDWORD old_flags);
#define OPEN_SIZE_WITHOUT_OVERLAYS (6 * sizeof(DWORD))
#define OPEN_SIZE_WITHOUT_FLAGS (5 * sizeof(DWORD))
#define INFO_SIZE_WITHOUT_OVERLAYS \
    (sizeof(BrainPresentInfo) - sizeof(((BrainPresentInfo *)0)->pOverlayBuffers))
#define STATS_SIZE_MINIMUM (6 * sizeof(DWORD))

static CRITICAL_SECTION s_lock;
static int s_ready;
static void *s_kernel[BRAIN_PRESENT_MAX_BUFFERS];
static void *s_user[BRAIN_PRESENT_MAX_BUFFERS];
static uint32_t s_physical[BRAIN_PRESENT_MAX_BUFFERS];
static DWORD s_buffer_bytes;
static DWORD s_buffers;
static DWORD s_owner;
static process_set_attributes_fn s_set_attributes;

static DWORD page_bytes(DWORD bytes) {
    return (bytes + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
}

static int cache_buffer(HANDLE owner, void *user, DWORD bytes) {
    DWORD first;
    DWORD end;

    if (user == NULL || bytes == 0) {
        return 0;
    }
    first = (DWORD)(uintptr_t)user & ~(PAGE_BYTES - 1);
    end = ((DWORD)(uintptr_t)user + bytes + PAGE_BYTES - 1) & ~(PAGE_BYTES - 1);
    if (end <= first) {
        return 0;
    }
    if (s_set_attributes == NULL) {
        s_set_attributes = (process_set_attributes_fn)GetProcAddress(
            GetModuleHandle(TEXT("coredll.dll")), TEXT("DirectHandleCall"));
    }
    if (s_set_attributes == NULL) {
        return 0;
    }
    return s_set_attributes(HANDLE_TYPE_PROCESS, PROCESS_SET_ATTRIBUTES, FALSE,
                            SET_ATTRIBUTES_ARGUMENTS, owner, (void *)(uintptr_t)first,
                            end - first, PAGE_ATTRIBUTES_WRITE_BACK, PAGE_ATTRIBUTES_CACHE_MASK,
                            NULL)
               ? 1
               : 0;
}

static void free_buffers(HANDLE owner) {
    for (DWORD buffer = 0; buffer < BRAIN_PRESENT_MAX_BUFFERS; buffer++) {
        if (s_user[buffer] != NULL && owner != NULL) {
            VirtualFreeEx(owner, s_user[buffer], 0, MEM_RELEASE);
        }
        if (s_kernel[buffer] != NULL) {
            FreePhysMem(s_kernel[buffer]);
        }
        s_user[buffer] = NULL;
        s_kernel[buffer] = NULL;
    }
    s_owner = 0;
    s_buffers = 0;
}

static void release_ended(void) {
    if (s_buffers != 0 && !brain_refresh_presenting()) {
        free_buffers(NULL);
    }
}

static int caller_owns(void) {
    return s_buffers != 0 && brain_refresh_presenting() && GetCallerVMProcessId() == s_owner;
}

static void *map_into_owner(HANDLE owner, uint32_t physical, DWORD bytes) {
    void *user = VirtualAllocEx(owner, NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);
    if (user == NULL) {
        return NULL;
    }
    if (!VirtualCopyEx(owner, user, GetCurrentProcess(), (void *)(physical >> 8), bytes,
                       PAGE_READWRITE | PAGE_PHYSICAL)) {
        VirtualFreeEx(owner, user, 0, MEM_RELEASE);
        return NULL;
    }
    return user;
}

static int alloc_mapped(HANDLE owner, DWORD bytes, void **kernel, void **user, uint32_t *physical) {
    ULONG phys = 0;
    *kernel = AllocPhysMem(bytes, PAGE_READWRITE | PAGE_NOCACHE, 0, 0, &phys);
    if (*kernel == NULL) {
        return 0;
    }
    *physical = (uint32_t)phys;
    memset(*kernel, 0, bytes);
    *user = map_into_owner(owner, *physical, bytes);
    return *user != NULL;
}

static int valid_open(const BrainPresentOpen *open) {
    if (open->Width == 0 || open->Height == 0) {
        return 0;
    }
    if (open->Width % PRESENT_BLOCK_PIXELS || open->Height % PRESENT_BLOCK_PIXELS) {
        return 0;
    }
    if (open->Scale != 1) {
        return 0;
    }
    if (open->Buffers < 1 || open->Buffers > BRAIN_PRESENT_MAX_BUFFERS) {
        return 0;
    }
    if (open->Overlays != 0) {
        return 0;
    }
    return 1;
}

static int present_open(const BrainPresentOpen *open, BrainPresentInfo *info, DWORD info_bytes) {
    brain_present_source source;
    HANDLE owner;
    int allocated = 1;

    if (!valid_open(open)) {
        return 0;
    }
    release_ended();
    if (s_buffers != 0) {
        return 0;
    }
    s_owner = GetCallerVMProcessId();
    owner = OpenProcess(0, FALSE, s_owner);
    if (owner == NULL) {
        s_owner = 0;
        return 0;
    }
    memset(&source, 0, sizeof(source));
    memset(info, 0, info_bytes);
    s_buffer_bytes = page_bytes(open->Width * open->Height * (DWORD)sizeof(uint16_t));
    s_buffers = open->Buffers;
    int cached = 1;
    for (DWORD buffer = 0; buffer < s_buffers && allocated; buffer++) {
        allocated = alloc_mapped(owner, s_buffer_bytes, &s_kernel[buffer], &s_user[buffer],
                                 &s_physical[buffer]);
        if (allocated) {
            cached &= cache_buffer(owner, s_user[buffer], s_buffer_bytes);
        }
        info->pBuffers[buffer] = (UINT16 *)s_user[buffer];
    }
    brain_log_value("present: cached", (unsigned long)cached);

    source.buffers = s_buffers;
    source.width = open->Width;
    source.height = open->Height;
    source.background = open->Background;
    source.flags = open->Flags;
    for (DWORD buffer = 0; buffer < s_buffers; buffer++) {
        source.physical[buffer] = s_physical[buffer];
    }
    if (!allocated || !brain_refresh_begin_present(&source, s_owner)) {
        brain_log("present: open failed");
        free_buffers(owner);
        CloseHandle(owner);
        memset(info, 0, info_bytes);
        return 0;
    }
    CloseHandle(owner);

    info->Stride = open->Width * (DWORD)sizeof(uint16_t);
    info->Width = open->Width;
    info->Height = open->Height;
    info->Scale = 1;
    brain_log_value("present: opened owner", s_owner);
    return 1;
}

static int present_frame(const BrainPresentFrame *frame, DWORD *replaced) {
    uint32_t taken = BRAIN_PRESENT_NONE;
    uint32_t start;

    if (!caller_owns() || frame->Buffer >= s_buffers) {
        return 0;
    }
    start = brain_us();
    CacheRangeFlush(s_user[frame->Buffer], s_buffer_bytes, CACHE_SYNC_WRITEBACK);
    brain_refresh_add_clean_time(brain_us() - start);
    if (!brain_refresh_queue_present(frame->Buffer, &taken)) {
        return 0;
    }
    if (replaced != NULL) {
        *replaced = taken;
    }
    return 1;
}

static int present_close(void) {
    HANDLE owner;

    if (s_buffers == 0 || GetCallerVMProcessId() != s_owner) {
        release_ended();
        return 0;
    }
    owner = OpenProcess(0, FALSE, s_owner);
    brain_refresh_end_present();
    free_buffers(owner);
    if (owner != NULL) {
        CloseHandle(owner);
    }
    brain_log("present: closed");
    return 1;
}

void brain_present_init(void) {
    InitializeCriticalSection(&s_lock);
    s_ready = 1;
}

int brain_present_supported(ULONG code) {
    return code >= BRAIN_PRESENT_OPEN && code <= BRAIN_PRESENT_WAIT_OVERLAY;
}

ULONG brain_present_escape(ULONG code, ULONG cjIn, PVOID pvIn, ULONG cjOut, PVOID pvOut) {
    ULONG result = 0;

    if (!s_ready) {
        return 0;
    }
    if (code == BRAIN_PRESENT_WAIT) {
        if (pvIn != NULL && cjIn == sizeof(DWORD) && caller_owns()) {
            result = brain_refresh_wait_present(*(DWORD *)pvIn) ? 1 : 0;
        }
        return result;
    }
    if (code == BRAIN_PRESENT_WAIT_OVERLAY) {
        return 0;
    }

    EnterCriticalSection(&s_lock);
    switch (code) {
    case BRAIN_PRESENT_OPEN:
        if (pvIn != NULL &&
            (cjIn == sizeof(BrainPresentOpen) || cjIn == OPEN_SIZE_WITHOUT_OVERLAYS ||
             cjIn == OPEN_SIZE_WITHOUT_FLAGS) &&
            pvOut != NULL && cjOut >= INFO_SIZE_WITHOUT_OVERLAYS) {
            BrainPresentOpen request;
            DWORD info_bytes = cjOut < sizeof(BrainPresentInfo) ? cjOut : (DWORD)sizeof(BrainPresentInfo);

            memset(&request, 0, sizeof(request));
            memcpy(&request, pvIn, cjIn);
            result = present_open(&request, (BrainPresentInfo *)pvOut, info_bytes) ? 1 : 0;
        }
        break;
    case BRAIN_PRESENT:
        if (pvIn != NULL && (cjIn == sizeof(DWORD) || cjIn == sizeof(BrainPresentFrame))) {
            BrainPresentFrame frame;

            memset(&frame, 0xFF, sizeof(frame));
            memcpy(&frame, pvIn, cjIn);
            result = present_frame(&frame,
                                   (pvOut != NULL && cjOut >= sizeof(DWORD)) ? (DWORD *)pvOut : NULL)
                         ? 1
                         : 0;
        }
        break;
    case BRAIN_PRESENT_CLOSE:
        result = present_close() ? 1 : 0;
        break;
    case BRAIN_PRESENT_STATS:
        if (pvOut != NULL && cjOut >= STATS_SIZE_MINIMUM) {
            BrainPresentStats stats;
            DWORD stats_bytes = cjOut < sizeof(stats) ? cjOut : (DWORD)sizeof(stats);

            brain_refresh_get_present_stats(&stats,
                                            pvIn != NULL && cjIn == sizeof(DWORD) && *(DWORD *)pvIn);
            memcpy(pvOut, &stats, stats_bytes);
            result = 1;
        }
        break;
    }
    LeaveCriticalSection(&s_lock);
    return result;
}
