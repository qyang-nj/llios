#include <stdio.h>

#include "util.h"

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
