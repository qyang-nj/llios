#include <stdio.h>

#include "macho_header.h"
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

struct load_command *get_load_command(void *base, uint32_t type) {
    struct mach_header_64 *mach_header = parse_mach_header(base);

    int offset = sizeof(struct mach_header_64);
    for (int i = 0; i < mach_header->ncmds; ++i) {
        struct load_command *lcmd = base + offset;

        if (lcmd->cmd == type) {
            return lcmd;
        }

        offset += lcmd->cmdsize;
    }

    return NULL;
}
