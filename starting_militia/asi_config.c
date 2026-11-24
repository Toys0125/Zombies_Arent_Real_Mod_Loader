#define WIN32_LEAN_AND_MEAN
#include "asi_config.h"
#include <bcrypt.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "asi_config_data.h"

enum { EXTRA_MAPS = 5, ENABLED = 6, LEGACY_CHANCE = 7, INI_LIMIT = 1024 * 1024 };

typedef struct {
    char value[VALUE_CAPACITY];
    const char *start, *end;
} Setting;

typedef struct {
    Setting fields[_countof(menu_keys)];
    const char *section_end;
} IniSettings;

static BOOL hash_data(HANDLE file, const void *data, DWORD size, BYTE digest[32])
{
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE hash;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, NULL, 0) < 0)
        return FALSE;
    BOOL ok = BCryptCreateHash(algorithm, &hash, NULL, 0, NULL, 0, 0) >= 0;
    if (ok) {
        if (file != INVALID_HANDLE_VALUE) {
            BYTE buffer[64 * 1024];
            DWORD count;
            while (ok) {
                ok = ReadFile(file, buffer, sizeof(buffer), &count, NULL);
                if (!ok || !count)
                    break;
                ok = BCryptHashData(hash, buffer, count, 0) >= 0;
            }
        } else {
            ok = BCryptHashData(hash, (BYTE *)data, size, 0) >= 0;
        }
        if (ok)
            ok = BCryptFinishHash(hash, digest, 32, 0) >= 0;
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok;
}

BOOL sha256_file(const wchar_t *path, BYTE digest[32])
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return FALSE;
    BOOL ok = hash_data(file, NULL, 0, digest);
    CloseHandle(file);
    return ok;
}

BOOL sha256_bytes(const void *data, DWORD size, BYTE digest[32])
{
    return hash_data(INVALID_HANDLE_VALUE, data, size, digest);
}

static char *read_ini(const wchar_t *path, DWORD *length)
{
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
            return NULL;
        *length = 0;
        return calloc(1, 1);
    }
    const DWORD size = GetFileSize(file, NULL);
    char *data = size <= INI_LIMIT ? malloc(size + 1) : NULL;
    if (data && (!ReadFile(file, data, size, length, NULL) || *length != size || memchr(data, 0, size))) {
        free(data);
        data = NULL;
    }
    if (data)
        data[size] = 0;
    CloseHandle(file);
    return data;
}

static const char *trim_left(const char *start, const char *end)
{
    while (start < end && isspace((unsigned char)*start))
        ++start;
    return start;
}

static const char *trim_right(const char *start, const char *end)
{
    while (end > start && isspace((unsigned char)end[-1]))
        --end;
    return end;
}

static BOOL parse_ini(const char *source, IniSettings *ini)
{
    const char *line = source;
    if (!strncmp(source, "\xef\xbb\xbf", 3))
        line += 3;
    BOOL militia = FALSE, had_section = FALSE;
    Setting *previous = NULL;
    size_t indent = 0;
    while (*line) {
        const char *next = strchr(line, '\n');
        const char *end = next ? next : line + strlen(line);
        const char *text = trim_left(line, end);
        const char *trimmed = trim_right(text, end);
        if (text < trimmed && *text != '#' && *text != ';') {
            if (*text == '[') {
                if (militia)
                    ini->section_end = line;
                militia = (size_t)(trimmed - text) == 9 && !memcmp(text, "[militia]", 9);
                if (militia && had_section)
                    return FALSE;
                had_section |= militia;
                previous = NULL;
            } else if (militia && previous && (size_t)(text - line) > indent) {
                size_t length = strlen(previous->value);
                size_t addition = (size_t)(trimmed - text);
                if (addition >= sizeof(previous->value) - length - 1)
                    return FALSE;
                previous->value[length++] = ' ';
                memcpy(previous->value + length, text, addition);
                previous->value[length + addition] = 0;
                previous->end = trimmed;
            } else if (militia) {
                previous = NULL;
                const char *separator = text;
                while (separator < trimmed && *separator != '=' && *separator != ':')
                    ++separator;
                if (separator == trimmed)
                    return FALSE;
                const size_t key_size = (size_t)(trim_right(text, separator) - text);
                for (unsigned i = 0; i < _countof(menu_keys); ++i) {
                    if (strlen(menu_keys[i]) != key_size || _strnicmp(text, menu_keys[i], key_size))
                        continue;
                    Setting *field = &ini->fields[i];
                    const char *value = trim_left(separator + 1, trimmed);
                    const size_t size = (size_t)(trimmed - value);
                    if (field->start || size >= sizeof(field->value))
                        return FALSE;
                    memcpy(field->value, value, size);
                    field->start = value;
                    field->end = trimmed;
                    previous = field;
                    indent = (size_t)(text - line);
                    break;
                }
            }
        }
        line = next ? next + 1 : end;
    }
    if (militia)
        ini->section_end = line;
    return had_section || !*source;
}

static BOOL boolean_value(const char *text, BOOL *value)
{
    if (!_stricmp(text, "true") || !_stricmp(text, "yes") || !_stricmp(text, "on") || !strcmp(text, "1"))
        *value = TRUE;
    else if (!_stricmp(text, "false") || !_stricmp(text, "no") || !_stricmp(text, "off") || !strcmp(text, "0"))
        *value = FALSE;
    else
        return FALSE;
    return TRUE;
}

static BOOL add_map_id(NativeSettings *settings, const char *name, size_t length)
{
    if (!length)
        return TRUE;
    if (length > 80 || settings->size + length + 2 > sizeof(settings->rules))
        return FALSE;
    for (size_t i = 0; i < length; ++i) {
        unsigned char ch = (unsigned char)name[i];
        if (!(ch >= 'a' && ch <= 'z') && !(ch >= 'A' && ch <= 'Z') &&
            !(ch >= '0' && ch <= '9') && ch != '_' && ch != '-')
            return FALSE;
    }
    for (DWORD offset = 25; offset < settings->size;) {
        const BYTE count = settings->rules[offset++];
        if (count == length && !memcmp(settings->rules + offset, name, length))
            return TRUE;
        offset += count;
    }
    settings->rules[settings->size++] = (BYTE)length;
    memcpy(settings->rules + settings->size, name, length);
    settings->size += (DWORD)length;
    return TRUE;
}

static BOOL profile(const IniSettings *ini, NativeSettings *settings)
{
    const Setting *fields = ini->fields;
    const unsigned threshold_order[] = {1, 2, 0}; // Rules are Elsewhere, Residential, Commercial.
    for (unsigned i = 0; i < 3; ++i) {
        const char *value = fields[i].start ? fields[i].value :
                            fields[LEGACY_CHANCE].start ? fields[LEGACY_CHANCE].value : chance_defaults[i];
        char *end;
        const double chance = strtod(value, &end);
        if (end == value || *end || !isfinite(chance) || chance < 0 || chance > 100 ||
            strspn(value, "0123456789+-.eE") != strlen(value))
            return FALSE;
        strcpy_s(settings->menu[i], VALUE_CAPACITY, value);
        const UINT64 threshold = (UINT64)(chance * (4294967296.0 / 100.0));
        memcpy(settings->rules + 8 * threshold_order[i], &threshold, sizeof(threshold));
    }
    BOOL all_maps = FALSE;
    settings->coop = FALSE;
    settings->enabled = TRUE;
    if ((fields[3].start && !boolean_value(fields[3].value, &all_maps)) ||
        (fields[4].start && !boolean_value(fields[4].value, &settings->coop)) ||
        (fields[ENABLED].start && !boolean_value(fields[ENABLED].value, &settings->enabled)))
        return FALSE;
    strcpy_s(settings->menu[3], VALUE_CAPACITY, all_maps ? "true" : "false");
    strcpy_s(settings->menu[4], VALUE_CAPACITY, settings->coop ? "true" : "false");
    strcpy_s(settings->menu[ENABLED], VALUE_CAPACITY, settings->enabled ? "true" : "false");
    settings->rules[24] = (BYTE)all_maps;
    settings->size = 25;
    for (unsigned i = 0; i < _countof(us_maps); ++i)
        if (!add_map_id(settings, us_maps[i], strlen(us_maps[i])))
            return FALSE;
    const char *map = fields[EXTRA_MAPS].value;
    strcpy_s(settings->menu[EXTRA_MAPS], VALUE_CAPACITY, map);
    while (*map) {
        const char *comma = strchr(map, ',');
        const char *end = comma ? comma : map + strlen(map);
        const char *start = trim_left(map, end);
        if (!add_map_id(settings, start, (size_t)(trim_right(start, end) - start)))
            return FALSE;
        map = comma ? comma + 1 : end;
    }
    settings->rules[settings->size++] = 0;
    return TRUE;
}

BOOL read_native_settings(const wchar_t *path, NativeSettings *settings)
{
    DWORD length;
    char *source = read_ini(path, &length);
    IniSettings *ini = calloc(1, sizeof(*ini));
    BOOL ok = source && ini && parse_ini(source, ini) && profile(ini, settings);
    free(ini);
    free(source);
    return ok;
}

static BOOL write_bytes(HANDLE file, const char *data, size_t size)
{
    DWORD written;
    return !size || (WriteFile(file, data, (DWORD)size, &written, NULL) && written == size);
}

static BOOL write_ini(HANDLE file, const char *source, DWORD length, const IniSettings *ini,
                      const char *const *changes)
{
    const char *cursor = source;
    unsigned written = 0;
    for (;;) {
        unsigned next = MENU_FIELDS;
        for (unsigned field = 0; field < MENU_FIELDS; ++field) {
            const char *position = ini->fields[field].start;
            if (!(written & (1u << field)) && changes[field] && position &&
                (next == MENU_FIELDS || position < ini->fields[next].start))
                next = field;
        }
        if (next == MENU_FIELDS)
            break;
        const Setting *field = &ini->fields[next];
        if (!write_bytes(file, cursor, (size_t)(field->start - cursor)) ||
            !write_bytes(file, changes[next], strlen(changes[next])))
            return FALSE;
        cursor = field->end;
        written |= 1u << next;
    }

    const char *section_end = ini->section_end ? ini->section_end : source + length;
    if (!write_bytes(file, cursor, (size_t)(section_end - cursor)))
        return FALSE;
    BOOL line_start = section_end == source || section_end[-1] == '\n';
    if (!ini->section_end && !write_bytes(file, "[militia]\r\n", 11))
        return FALSE;
    for (unsigned field = 0; field < MENU_FIELDS; ++field) {
        if (!changes[field] || ini->fields[field].start)
            continue;
        if ((!line_start && !write_bytes(file, "\r\n", 2)) ||
            !write_bytes(file, menu_keys[field], strlen(menu_keys[field])) ||
            !write_bytes(file, " = ", 3) ||
            !write_bytes(file, changes[field], strlen(changes[field])) ||
            !write_bytes(file, "\r\n", 2))
            return FALSE;
        line_start = TRUE;
    }
    return write_bytes(file, section_end, (size_t)(source + length - section_end));
}

BOOL save_native_settings(const wchar_t *path, const char *const *changes)
{
    DWORD length;
    char *source = read_ini(path, &length);
    IniSettings *ini = calloc(1, sizeof(*ini));
    BOOL ok = FALSE;
    if (source && ini && parse_ini(source, ini)) {
        wchar_t directory[PATH_CAPACITY], temporary[PATH_CAPACITY];
        wcscpy_s(directory, _countof(directory), path);
        wchar_t *separator = wcsrchr(directory, L'\\');
        if (separator) {
            *separator = 0;
            if (GetTempFileNameW(directory, L"zar", 0, temporary)) {
                HANDLE file = CreateFileW(temporary, GENERIC_WRITE, 0, NULL, TRUNCATE_EXISTING, 0, NULL);
                if (file != INVALID_HANDLE_VALUE) {
                    ok = write_ini(file, source, length, ini, changes) && FlushFileBuffers(file);
                    CloseHandle(file);
                }
                NativeSettings check;
                ok = ok && read_native_settings(temporary, &check) &&
                     (!check.enabled || !check.coop || check.size <= COOP_RULES_LIMIT) &&
                     MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
                if (!ok)
                    DeleteFileW(temporary);
            }
        }
    }
    free(ini);
    free(source);
    return ok;
}
