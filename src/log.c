/*
 * log.c - Debug log. Buffers lines in memory and batches them to a file on the
 * SD card once a second; OutputDebugString is gated behind the Log setting
 * because the debug UART blocks. Keeps the hot path off slow I/O.
 */
#include <windows.h>

#include "log.h"

static const WCHAR brain_log_path[] = L"\\Storage Card\\brain-display.log";
static const WCHAR brain_log_prev[] = L"\\Storage Card\\brain-display-prev.log";

static char brain_log_buffer[16384];
static DWORD brain_log_length;
static int brain_log_file_ready;
static volatile LONG brain_log_suspended;
static volatile LONG brain_log_debug_enabled;
static volatile LONG brain_log_hold_until;

static int file_io_held(void) {
    LONG until = brain_log_hold_until;
    if (until == 0) {
        return 0;
    }
    if ((LONG)(GetTickCount() - until) >= 0) {
        brain_log_hold_until = 0;
        return 0;
    }
    return 1;
}

void brain_log_hold_file_io(DWORD milliseconds) {
    LONG until = (LONG)(GetTickCount() + milliseconds);
    if (until == 0) {
        until = 1;
    }
    brain_log_hold_until = until;
}

static DWORD ascii_length(const char *text) {
    DWORD length = 0;
    while (text[length] != 0) {
        length++;
    }
    return length;
}

static void emit_debug(const char *line) {
    WCHAR wide[256];
    DWORD index = 0;
    while (line[index] != 0 && index < 254) {
        wide[index] = (WCHAR)(unsigned char)line[index];
        index++;
    }
    wide[index++] = L'\n';
    wide[index] = 0;
    OutputDebugStringW(wide);
}

static void buffer_line(const char *line) {
    DWORD length = ascii_length(line);
    if (brain_log_length + length + 2 > sizeof(brain_log_buffer)) {
        return;
    }
    for (DWORD index = 0; index < length; index++) {
        brain_log_buffer[brain_log_length++] = line[index];
    }
    brain_log_buffer[brain_log_length++] = '\r';
    brain_log_buffer[brain_log_length++] = '\n';
}

static void write_handle(HANDLE file, const char *data, DWORD length) {
    DWORD written = 0;
    WriteFile(file, data, length, &written, NULL);
}

static void append_buffered(void) {
    if (brain_log_length == 0) {
        return;
    }
    HANDLE file = CreateFileW(brain_log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        brain_log_hold_file_io(2000);
        return;
    }
    SetFilePointer(file, 0, NULL, FILE_END);
    write_handle(file, brain_log_buffer, brain_log_length);
    FlushFileBuffers(file);
    CloseHandle(file);
    brain_log_length = 0;
}

static void open_file_ready(void) {
    if (brain_log_file_ready || brain_log_suspended) {
        return;
    }
    if (GetFileAttributesW(L"\\Storage Card") == 0xFFFFFFFF) {
        return;
    }
    DeleteFileW(brain_log_prev);
    MoveFileW(brain_log_path, brain_log_prev);
    HANDLE file = CreateFileW(brain_log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
        write_handle(file, brain_log_buffer, brain_log_length);
        FlushFileBuffers(file);
        CloseHandle(file);
        brain_log_length = 0;
        brain_log_file_ready = 1;
    } else {
        brain_log_hold_file_io(2000);
    }
}

static void emit(const char *line) {
    if (brain_log_debug_enabled) {
        emit_debug(line);
    }
    buffer_line(line);
    if (!brain_log_file_ready && !brain_log_suspended && !file_io_held()) {
        open_file_ready();
    }
}

void brain_log(const char *message) {
    emit(message);
}

/* Buffer a line to memory only, never touching the debug UART or the file. Safe
   to call from the power-off handler, where OutputDebugString and file I/O yield
   the boosted handler thread and trip the kernel's power-handler guard. */
void brain_log_buffered(const char *message) {
    buffer_line(message);
}

void brain_log_debug(const char *message) {
    if (brain_log_debug_enabled) {
        emit_debug(message);
    }
}

void brain_log_set_suspended(int suspended) {
    InterlockedExchange(&brain_log_suspended, suspended ? 1 : 0);
}

void brain_log_set_debug(int enabled) {
    InterlockedExchange(&brain_log_debug_enabled, enabled ? 1 : 0);
}

int brain_log_debug_active(void) {
    return brain_log_debug_enabled != 0;
}

void brain_log_flush(void) {
    if (brain_log_suspended || file_io_held()) {
        return;
    }
    if (!brain_log_file_ready) {
        open_file_ready();
        return;
    }
    append_buffered();
}

void brain_log_value(const char *label, unsigned long value) {
    static const char digits[] = "0123456789ABCDEF";
    char line[288];
    DWORD index = 0;
    DWORD label_length = ascii_length(label);
    while (index < label_length && index < 270) {
        line[index] = label[index];
        index++;
    }
    line[index++] = ' ';
    line[index++] = '0';
    line[index++] = 'x';
    for (int shift = 0; shift < 8; shift++) {
        line[index++] = digits[(value >> ((7 - shift) * 4)) & 0xF];
    }
    line[index] = 0;
    emit(line);
}
