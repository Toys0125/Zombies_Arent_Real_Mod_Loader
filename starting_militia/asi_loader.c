#define WIN32_LEAN_AND_MEAN
#include "asi_config.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

enum { PATCH_CAPACITY = 8 * 1024 * 1024, MAX_EDITS = 128 };
enum { TARGET_IMAGE, TARGET_CODE, TARGET_STATE, TARGET_DYNAMIC };
enum { GIS_COUNT, GIS_MAPS, MILITIA_RULES, COOP_ENABLED, RULES_SIZE, INITIAL_SETTINGS, DYNAMIC_COUNT };

typedef struct { DWORD offset, target; BYTE region, kind; } Relocation;
typedef struct { DWORD offset, stock_rva; } StockChain;
typedef struct { DWORD offset, count; } PdataRange;
typedef struct { DWORD rva; BYTE reg; } ReadySite;
typedef struct {
    DWORD rva, target;
    BYTE length, opcode_length, settings;
    BYTE opcode[2], expected[32];
} Hook;
typedef struct {
    const char *name;
    const wchar_t *map, *cache;
    BYTE alternate_from[32], alternate_to[32];
} GisSource;
typedef struct {
    const char *version;
    BYTE digest[32], hello_build[8], ready_seed[8];
    const BYTE *code;
    DWORD code_size;
    const Relocation *relocations;
    DWORD relocation_count;
    const StockChain *chains;
    DWORD chain_count;
    const RUNTIME_FUNCTION *frames;
    DWORD frame_count;
    const PdataRange *pdata;
    DWORD pdata_count;
    const Hook *hooks;
    DWORD hook_count;
    const DWORD *hello_sites;
    DWORD hello_count;
    const ReadySite *ready_sites;
    DWORD ready_count;
} NativeBuild;
typedef struct {
    DWORD rva, length;
    BYTE after[32];
    DWORD protection;
} Edit;
typedef struct {
    BYTE *code;
    DWORD used;
} Payload;

#include "asi_payload.h"

static wchar_t settings_path[PATH_CAPACITY];

static int save_settings(const char *const *changes)
{
    return save_native_settings(settings_path, changes);
}

static void log_message(HANDLE log, const char *message)
{
    if (log != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(log, message, (DWORD)strlen(message), &written, NULL);
    }
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

static BOOL apply_patch(BYTE *image, BYTE *linked, DWORD code_size, RUNTIME_FUNCTION *functions,
                        DWORD function_count, Edit *edits, DWORD edit_count)
{
    DWORD unused;
    if (!VirtualProtect(linked, code_size, PAGE_EXECUTE_READ, &unused))
        return FALSE;
    if (!RtlAddFunctionTable(functions, function_count, (DWORD64)image))
        return FALSE;

    DWORD writable = 0;
    for (; writable < edit_count; ++writable) {
        Edit *edit = &edits[writable];
        if (!VirtualProtect(image + edit->rva, edit->length, PAGE_EXECUTE_READWRITE, &edit->protection))
            break;
    }
    const BOOL ready = writable == edit_count;
    if (ready) {
        for (DWORD i = 0; i < edit_count; ++i)
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

static BOOL append(Payload *payload, const void *data, DWORD size)
{
    if (size > PATCH_CAPACITY - CODE_OFFSET - payload->used)
        return FALSE;
    if (data)
        memcpy(payload->code + payload->used, data, size);
    payload->used += size;
    return TRUE;
}

static BOOL align_payload(Payload *payload, DWORD alignment)
{
    return append(payload, NULL, (0u - payload->used) & (alignment - 1));
}

static BOOL append_map(Payload *payload, const wchar_t *directory, const GisSource *source, DWORD *count)
{
    wchar_t cache[PATH_CAPACITY], map[PATH_CAPACITY];
    if (swprintf_s(cache, _countof(cache), L"%ls/starting_militia/%ls", directory, source->cache) < 0 ||
        swprintf_s(map, _countof(map), L"%ls/maps/%ls", directory, source->map) < 0)
        return FALSE;
    if (GetFileAttributesW(map) == INVALID_FILE_ATTRIBUTES)
        return TRUE;
    HANDLE file = CreateFileW(cache, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return GetLastError() == ERROR_FILE_NOT_FOUND || GetLastError() == ERROR_PATH_NOT_FOUND;

    struct { char magic[4]; BYTE hash[32]; DWORD count; } header;
    DWORD size = GetFileSize(file, NULL), read;
    BOOL ok = size >= sizeof(header) && size <= PATCH_CAPACITY &&
              ReadFile(file, &header, sizeof(header), &read, NULL) && read == sizeof(header);
    if (ok) {
        size -= sizeof(header);
        ok = !memcmp(header.magic, "SFG1", 4) && header.count <= size && size <= 5ull * header.count;
    }
    BYTE installed[32];
    if (ok)
        ok = sha256_file(map, installed);
    if (ok && header.count && (!memcmp(header.hash, installed, 32) ||
        (!memcmp(header.hash, source->alternate_from, 32) && !memcmp(installed, source->alternate_to, 32)))) {
        struct { DWORD count, size; char name[80]; } entry = {header.count, size, {0}};
        strcpy_s(entry.name, _countof(entry.name), source->name);
        ok = append(payload, &entry, sizeof(entry));
        BYTE *destination = payload->code + payload->used;
        ok = ok && append(payload, NULL, size) &&
             ReadFile(file, destination, size, &read, NULL) && read == size && align_payload(payload, 4);
        if (ok)
            ++*count;
    }
    CloseHandle(file);
    return ok;
}

static BOOL prepare_data(Payload *payload, const NativeBuild *build, const NativeSettings *settings,
                         const wchar_t *directory, DWORD dynamic[DYNAMIC_COUNT], BYTE coop_digest[32])
{
    if (!append(payload, build->code, build->code_size) || !align_payload(payload, 4))
        return FALSE;
    dynamic[GIS_COUNT] = payload->used;
    if (!append(payload, NULL, sizeof(DWORD)))
        return FALSE;
    dynamic[GIS_MAPS] = payload->used;
    DWORD maps = 0;
    if (settings->enabled) {
        for (unsigned i = 0; i < _countof(gis_sources); ++i)
            if (!append_map(payload, directory, &gis_sources[i], &maps))
                return FALSE;
    }
    memcpy(payload->code + dynamic[GIS_COUNT], &maps, sizeof(maps));
    if (settings->enabled && settings->coop && !sha256_bytes(payload->code, payload->used, coop_digest))
        return FALSE;
    if (!align_payload(payload, 8))
        return FALSE;
    dynamic[MILITIA_RULES] = payload->used;
    if (!append(payload, settings->rules, settings->size))
        return FALSE;
    const BYTE coop = (BYTE)settings->coop;
    dynamic[COOP_ENABLED] = payload->used;
    if (!append(payload, &coop, 1) || !align_payload(payload, 4))
        return FALSE;
    dynamic[RULES_SIZE] = payload->used;
    if (!append(payload, &settings->size, sizeof(settings->size)))
        return FALSE;
    dynamic[INITIAL_SETTINGS] = payload->used;
    for (unsigned i = 0; i < MENU_FIELDS; ++i) {
        if (!append(payload, settings->menu[i], (DWORD)strlen(settings->menu[i]) + 1))
            return FALSE;
    }
    return align_payload(payload, 4);
}

static void relocate(Payload *payload, const NativeBuild *build, DWORD state_rva, const DWORD *dynamic)
{
    const DWORD code_rva = state_rva + CODE_OFFSET;
    for (DWORD i = 0; i < build->relocation_count; ++i) {
        const Relocation *rel = &build->relocations[i];
        DWORD target = rel->target;
        switch (rel->region) {
        case TARGET_CODE: target += code_rva; break;
        case TARGET_STATE: target += state_rva; break;
        case TARGET_DYNAMIC: target = code_rva + dynamic[target]; break;
        }
        DWORD value;
        memcpy(&value, payload->code + rel->offset, sizeof(value));
        value += target;
        if (rel->kind != 3)
            value -= code_rva + rel->offset + rel->kind;
        memcpy(payload->code + rel->offset, &value, sizeof(value));
    }
}

static int compare_frames(const void *a, const void *b)
{
    const DWORD left = ((const RUNTIME_FUNCTION *)a)->BeginAddress;
    const DWORD right = ((const RUNTIME_FUNCTION *)b)->BeginAddress;
    return (left > right) - (left < right);
}

static BOOL prepare_unwind(Payload *payload, const NativeBuild *build, const BYTE *image, DWORD code_rva,
                           RUNTIME_FUNCTION **functions, DWORD *count)
{
    for (DWORD i = 0; i < build->chain_count; ++i) {
        const StockChain *chain = &build->chains[i];
        DWORD64 owner;
        const RUNTIME_FUNCTION *stock = RtlLookupFunctionEntry((DWORD64)(image + chain->stock_rva), &owner, NULL);
        if (!stock || owner != (DWORD64)image)
            return FALSE;
        memcpy(payload->code + chain->offset, stock, sizeof(*stock));
    }

    *functions = (RUNTIME_FUNCTION *)(payload->code + payload->used);
    *count = build->frame_count;
    for (DWORD i = 0; i < build->frame_count; ++i) {
        RUNTIME_FUNCTION frame = build->frames[i];
        frame.BeginAddress += code_rva;
        frame.EndAddress += code_rva;
        frame.UnwindData += code_rva;
        if (!append(payload, &frame, sizeof(frame)))
            return FALSE;
    }
    for (DWORD i = 0; i < build->pdata_count; ++i) {
        const PdataRange *range = &build->pdata[i];
        if (!append(payload, payload->code + range->offset, range->count * sizeof(RUNTIME_FUNCTION)))
            return FALSE;
        *count += range->count;
    }
    qsort(*functions, *count, sizeof(RUNTIME_FUNCTION), compare_frames);
    return TRUE;
}

static BOOL add_edit(Edit *edits, DWORD *count, const BYTE *image, DWORD image_size,
                     DWORD rva, DWORD length, const BYTE *expected, const BYTE *after)
{
    if (*count == MAX_EDITS || rva >= image_size || length > image_size - rva ||
        length > sizeof(edits[0].after) || memcmp(image + rva, expected, length))
        return FALSE;
    Edit *edit = &edits[(*count)++];
    edit->rva = rva;
    edit->length = length;
    memcpy(edit->after, after, length);
    return TRUE;
}

static BOOL prepare_edits(Edit *edits, DWORD *count, const NativeBuild *build, const NativeSettings *settings,
                          const BYTE *image, DWORD image_size, DWORD state_rva, BYTE coop_digest[32])
{
    const DWORD code_rva = state_rva + CODE_OFFSET;
    if (settings->enabled && settings->coop) {
        if (!memcmp(coop_digest, build->hello_build, 8))
            memset(coop_digest, 0, 8);
        for (DWORD i = 0; i < build->hello_count; ++i) {
            if (!add_edit(edits, count, image, image_size, build->hello_sites[i], 8, build->hello_build, coop_digest))
                return FALSE;
        }
        for (DWORD i = 0; i < build->ready_count; ++i) {
            const ReadySite *site = &build->ready_sites[i];
            BYTE before[10] = {0x48, (BYTE)(0xb8 + site->reg)};
            memcpy(before + 2, build->ready_seed, 8);
            BYTE after[10] = {0x48, 0x8b, (BYTE)(5 + 8 * site->reg), 0, 0, 0, 0, 0x90, 0x90, 0x90};
            const DWORD disp = state_rva + READY_SEED_OFFSET - (site->rva + 7);
            memcpy(after + 3, &disp, sizeof(disp));
            if (!add_edit(edits, count, image, image_size, site->rva, sizeof(before), before, after))
                return FALSE;
        }
    }
    for (DWORD i = 0; i < build->hook_count; ++i) {
        const Hook *hook = &build->hooks[i];
        if (!settings->enabled && !hook->settings)
            continue;
        BYTE after[32];
        memset(after, 0x90, sizeof(after));
        memcpy(after, hook->opcode, hook->opcode_length);
        const DWORD disp = code_rva + hook->target - (hook->rva + hook->opcode_length + 4);
        memcpy(after + hook->opcode_length, &disp, sizeof(disp));
        if (!add_edit(edits, count, image, image_size, hook->rva, hook->length, hook->expected, after))
            return FALSE;
    }
    return TRUE;
}

static BOOL install_patch(BYTE *image, DWORD image_size, BYTE *state, const NativeBuild *build,
                          const NativeSettings *settings, const wchar_t *directory)
{
    const DWORD state_rva = (DWORD)((uintptr_t)state - (uintptr_t)image);
    const DWORD code_rva = state_rva + CODE_OFFSET;
    Payload payload = {state + CODE_OFFSET, 0};
    DWORD dynamic[DYNAMIC_COUNT];
    BYTE coop_digest[32];
    Edit edits[MAX_EDITS];
    DWORD edit_count = 0, function_count;
    RUNTIME_FUNCTION *functions;
    int (*save_callback)(const char *const *) = save_settings;
    memcpy(state + SAVE_OFFSET, &save_callback, sizeof(save_callback));

    if (!prepare_data(&payload, build, settings, directory, dynamic, coop_digest) ||
        !prepare_edits(edits, &edit_count, build, settings, image, image_size, state_rva, coop_digest))
        return FALSE;
    relocate(&payload, build, state_rva, dynamic);
    return prepare_unwind(&payload, build, image, code_rva, &functions, &function_count) &&
           apply_patch(image, payload.code, payload.used, functions, function_count, edits, edit_count);
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
    BYTE hash[32];
    const BOOL hashed = sha256_file(directory, hash);
    *name = 0;
    if (swprintf_s(settings_path, _countof(settings_path), L"%ls\\starting_militia\\settings.ini", directory) < 0 ||
        swprintf_s(log_path, _countof(log_path), L"%ls\\starting_militia\\asi.log", directory) < 0)
        return;

    HANDLE log = CreateFileW(log_path, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, 0, NULL);
    const NativeBuild *build = NULL;
    if (hashed) {
        for (unsigned i = 0; i < _countof(native_builds); ++i)
            if (!memcmp(hash, native_builds[i].digest, 32))
                build = &native_builds[i];
    }
    NativeSettings settings;
    if (!build) {
        log_message(log, "Unsupported game executable. Armed Citizens ASI skipped.\r\n");
    } else if (!read_native_settings(settings_path, &settings) ||
               (settings.enabled && settings.coop && settings.size > COOP_RULES_LIMIT)) {
        log_message(log, "Invalid settings.ini. Armed Citizens ASI skipped.\r\n");
    } else {
        BYTE *image = (BYTE *)GetModuleHandleW(NULL);
        const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)image;
        const IMAGE_NT_HEADERS64 *nt = (const IMAGE_NT_HEADERS64 *)(image + dos->e_lfanew);
        const DWORD image_size = nt->OptionalHeader.SizeOfImage;
        BYTE *state = reserve_patch_space(image, image_size);
        HMODULE self;
        const BOOL installed = state &&
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                               (const wchar_t *)&InitializeASI, &self) &&
            install_patch(image, image_size, state, build, &settings, directory);
        if (installed) {
            char message[128];
            sprintf_s(message, _countof(message), "Prepared Armed Citizens Mod for v%s: %s.\r\nArmed Citizens ASI applied.\r\n",
                      build->version, settings.enabled ? "gameplay and settings" : "settings only (mod disabled)");
            log_message(log, message);
        } else {
            if (state)
                VirtualFree(state, 0, MEM_RELEASE);
            log_message(log, "Could not link the mod, verify GIS data, or apply the hooks. Armed Citizens ASI skipped.\r\n");
        }
    }
    if (log != INVALID_HANDLE_VALUE)
        CloseHandle(log);
}
