#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

enum { PATCH_CAPACITY = 8 * 1024 * 1024, MAX_EDITS = 128, PATH_CAPACITY = 1024 };

typedef struct {
    char magic[8];
    DWORD state_rva, code_offset, code_size, functions_offset, functions_count, edit_count;
} PatchHeader;

typedef struct {
    DWORD rva, length;
    const BYTE *after;
    DWORD protection;
} Edit;

static BOOL run_builder(const wchar_t *directory, const wchar_t *patch_file, DWORD rva, HANDLE log)
{
    wchar_t builder[PATH_CAPACITY];
    wchar_t command[PATH_CAPACITY * 2 + 64];
    if (swprintf_s(builder, _countof(builder), L"%ls\\starting_militia\\armed-citizens-launcher.exe", directory) < 0 ||
        swprintf_s(command, _countof(command), L"\"%ls\" --build-asi-patch %lx \"%ls\"",
                   builder, rva, patch_file) < 0)
        return FALSE;

    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = startup.hStdError = log;
    PROCESS_INFORMATION process;
    if (!CreateProcessW(builder, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, directory,
                        &startup, &process))
        return FALSE;

    DWORD exit_code = 1;
    if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0)
        GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return exit_code == 0;
}

static BYTE *reserve_patch_space(const BYTE *image, DWORD image_size)
{
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    const uintptr_t granularity = info.dwAllocationGranularity;
    const uintptr_t limit = (uintptr_t)image + 0x70000000u;
    uintptr_t address = (uintptr_t)image + image_size;
    MEMORY_BASIC_INFORMATION region;
    while (address < limit && VirtualQuery((void *)address, &region, sizeof(region))) {
        const uintptr_t end = (uintptr_t)region.BaseAddress + region.RegionSize;
        const uintptr_t start = (address + granularity - 1) & ~(granularity - 1);
        if (region.State == MEM_FREE && start + PATCH_CAPACITY <= end && start + PATCH_CAPACITY < limit) {
            BYTE *result = VirtualAlloc((void *)start, PATCH_CAPACITY, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (result)
                return result;
        }
        address = end;
    }
    return NULL;
}

static BOOL apply_patch(BYTE *image, DWORD image_size, BYTE *state, const BYTE *patch, DWORD size)
{
    PatchHeader header;
    if (size < sizeof(header))
        return FALSE;
    memcpy(&header, patch, sizeof(header));
    const BYTE *code = patch + sizeof(header);
    if (memcmp(header.magic, "ZARASI1", 8) || header.state_rva != (uintptr_t)state - (uintptr_t)image ||
        header.code_offset < 4096 || header.code_offset % 4096 ||
        header.code_offset >= PATCH_CAPACITY || header.code_size > PATCH_CAPACITY - header.code_offset ||
        header.code_size > size - sizeof(header) || header.functions_offset > header.code_size ||
        !header.functions_count || header.functions_count >
            (header.code_size - header.functions_offset) / sizeof(RUNTIME_FUNCTION) ||
        !header.edit_count || header.edit_count > MAX_EDITS)
        return FALSE;

    Edit edits[MAX_EDITS];
    const BYTE *next = code + header.code_size;
    const BYTE *end = patch + size;
    for (DWORD i = 0; i < header.edit_count; ++i) {
        DWORD fields[2];
        if ((size_t)(end - next) < sizeof(fields))
            return FALSE;
        memcpy(fields, next, sizeof(fields));
        next += sizeof(fields);
        const DWORD rva = fields[0], length = fields[1];
        if (rva >= image_size || !length || length > image_size - rva ||
            length > (size_t)(end - next) / 2 || memcmp(image + rva, next, length))
            return FALSE;
        edits[i] = (Edit){rva, length, next + length, 0};
        next += 2 * length;
    }
    if (next != end)
        return FALSE;

    BYTE *linked = state + header.code_offset;
    memcpy(linked, code, header.code_size);
    DWORD unused;
    if (!VirtualProtect(linked, header.code_size, PAGE_EXECUTE_READ, &unused))
        return FALSE;
    RUNTIME_FUNCTION *functions = (RUNTIME_FUNCTION *)(linked + header.functions_offset);
    if (!RtlAddFunctionTable(functions, header.functions_count, (DWORD64)image))
        return FALSE;

    DWORD writable = 0;
    for (; writable < header.edit_count; ++writable) {
        Edit *edit = &edits[writable];
        if (!VirtualProtect(image + edit->rva, edit->length, PAGE_EXECUTE_READWRITE, &edit->protection))
            break;
    }
    const BOOL ready = writable == header.edit_count;
    if (ready) {
        for (DWORD i = 0; i < header.edit_count; ++i)
            memcpy(image + edits[i].rva, edits[i].after, edits[i].length);
        FlushInstructionCache(GetCurrentProcess(), NULL, 0);
    } else {
        RtlDeleteFunctionTable(functions);
    }
    // More than one edit can share a page. Unwind the protection changes in reverse order.
    while (writable) {
        const Edit *edit = &edits[--writable];
        VirtualProtect(image + edit->rva, edit->length, edit->protection, &unused);
    }
    return ready;
}

static BOOL install_patch(BYTE *image, DWORD image_size, const wchar_t *directory, HANDLE log)
{
    wchar_t temp[PATH_CAPACITY], path[PATH_CAPACITY];
    DWORD temp_length = GetTempPathW(_countof(temp), temp);
    if (!temp_length || temp_length >= MAX_PATH || !GetTempFileNameW(temp, L"zar", 0, path))
        return FALSE;

    BYTE *state = reserve_patch_space(image, image_size);
    BOOL installed = FALSE;
    if (state && run_builder(directory, path, (DWORD)((uintptr_t)state - (uintptr_t)image), log)) {
        HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        if (file != INVALID_HANDLE_VALUE) {
            DWORD size = GetFileSize(file, NULL), read = 0;
            if (size && size <= PATCH_CAPACITY) {
                BYTE *patch = HeapAlloc(GetProcessHeap(), 0, size);
                if (patch) {
                    if (ReadFile(file, patch, size, &read, NULL) && read == size)
                        installed = apply_patch(image, image_size, state, patch, size);
                    HeapFree(GetProcessHeap(), 0, patch);
                }
            }
            CloseHandle(file);
        }
    }
    DeleteFileW(path);
    if (state && !installed)
        VirtualFree(state, 0, MEM_RELEASE);
    return installed;
}

__declspec(dllexport) void InitializeASI(void)
{
    static LONG initialized;
    if (InterlockedExchange(&initialized, 1))
        return;

    wchar_t directory[PATH_CAPACITY], log_path[PATH_CAPACITY];
    const DWORD length = GetModuleFileNameW(NULL, directory, _countof(directory));
    if (!length || length >= _countof(directory))
        return;
    wchar_t *name = wcsrchr(directory, L'\\');
    if (!name || _wcsicmp(name + 1, L"zombies-arent-real.exe"))
        return;
    *name = 0;
    if (swprintf_s(log_path, _countof(log_path), L"%ls\\starting_militia\\asi.log", directory) < 0)
        return;

    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), NULL, TRUE};
    HANDLE log = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes,
                             CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (log == INVALID_HANDLE_VALUE)
        return;
    BYTE *image = (BYTE *)GetModuleHandleW(NULL);
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image;
    const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
    const BOOL installed = install_patch(image, nt->OptionalHeader.SizeOfImage, directory, log);
    const char *message = installed ? "Armed Citizens ASI applied.\r\n" :
                                      "Armed Citizens ASI skipped. Check the builder output above and the installed files.\r\n";
    DWORD written;
    WriteFile(log, message, (DWORD)strlen(message), &written, NULL);
    CloseHandle(log);
}
