#ifndef ARMED_CITIZENS_ASI_CONFIG_H
#define ARMED_CITIZENS_ASI_CONFIG_H

#include <windows.h>
#include "asi_config_limits.h"

enum { PATH_CAPACITY = 1024, VALUE_CAPACITY = 4096, RULES_BUFFER_SIZE = 8192 };

typedef struct {
    BYTE rules[RULES_BUFFER_SIZE];
    DWORD size;
    BOOL enabled, coop;
    char menu[MENU_FIELDS][VALUE_CAPACITY];
} NativeSettings;

BOOL read_native_settings(const wchar_t *path, NativeSettings *settings);
BOOL save_native_settings(const wchar_t *path, const char *const *changes);
BOOL sha256_file(const wchar_t *path, BYTE digest[32]);
BOOL sha256_bytes(const void *data, DWORD size, BYTE digest[32]);

#endif
