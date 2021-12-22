#include <stdio.h>

extern "C" {
#include "util.h"
}

#include "encryption_info.h"

void printEncryptionInfo(uint8_t *base, struct encryption_info_command_64 *cmd) {
    char readable_size [16];
    format_size(cmd->cryptsize, readable_size);

    printf("%-20s cmdsize: %-5u cryptoff: %u  cryptsize: %u(%s)  cryptid: %u   pad: %u\n",
        "LC_ENCRYPTION_INFO_64", cmd->cmdsize, cmd->cryptoff, cmd->cryptsize, readable_size, cmd->cryptid, cmd->pad);
}
