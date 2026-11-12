#include <windows.h>

#include "log.h"

static const WCHAR brain_log_path[] = L"\\Storage Card\\brain-display.log";
static const WCHAR brain_log_prev[] = L"\\Storage Card\\brain-display-prev.log";

static char brain_log_buffer[16384];
static DWORD brain_log_length;
static int brain_log_file_ready;
static volatile LONG brain_log_suspended;

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
    }
}

static void append_line(const char *line, DWORD length) {
    HANDLE file = CreateFileW(brain_log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                              OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    SetFilePointer(file, 0, NULL, FILE_END);
    write_handle(file, line, length);
    write_handle(file, "\r\n", 2);
    FlushFileBuffers(file);
    CloseHandle(file);
}

static void emit(const char *line) {
    emit_debug(line);
    if (brain_log_suspended) {
        buffer_line(line);
        return;
    }
    if (!brain_log_file_ready) {
        buffer_line(line);
        open_file_ready();
        return;
    }
    append_buffered();
    append_line(line, ascii_length(line));
}

void brain_log(const char *message) {
    emit(message);
}

void brain_log_debug(const char *message) {
    emit_debug(message);
}

void brain_log_set_suspended(int suspended) {
    InterlockedExchange(&brain_log_suspended, suspended ? 1 : 0);
}

void brain_log_flush(void) {
    if (brain_log_file_ready && !brain_log_suspended) {
        append_buffered();
    }
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
