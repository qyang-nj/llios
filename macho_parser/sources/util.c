#include <stdio.h>

#include "util.h"

int read_uleb128(const uint8_t *p, uint64_t *out) {
    uint64_t result = 0;
    int i = 0;

    do {
        uint8_t byte = *p & 0x7f;
        result |= byte << (i * 7);
        i++;
    } while (*p++ & 0x80);

    *out = result;
    return i;
}

void format_string(char *str, char *formatted) {
    int j = 0;
    for (int i = 0; str[i] != '\0'; ++i) {
        switch(str[i]) {
            case '\n':
                formatted[j++] = '\\';
                formatted[j++] = 'n';
                break;
            default:
                formatted[j++] = str[i];
                break;
        }
    }
    formatted[j] = '\0';
}

void format_hex(void *buffer, size_t size, char *formatted) {
    for (int i = 0; i < size; ++i) {
        sprintf(formatted + i * 2, "%02x", *((uint8_t *)buffer + i));
    }
}

void format_size(uint64_t size_in_byte, char *formatted) {
    if (size_in_byte < 1024) {
        sprintf(formatted, "%lluB", size_in_byte);
    } else if (size_in_byte / 1024 < 1024) {
        sprintf(formatted, "%.2fKB", (double)size_in_byte / 1024 );
    } else if (size_in_byte / 1024 / 1204 < 1024) {
        sprintf(formatted, "%.2fMB", (double)size_in_byte / 1024 / 1024);
    } else {
        sprintf(formatted, "%.2fGB", (double)size_in_byte / 1024 / 1024 / 1024);
    }
}
