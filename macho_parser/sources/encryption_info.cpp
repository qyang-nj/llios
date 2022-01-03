#include <stdio.h>

#include "encryption_info.h"

void printEncryptionInfo(uint8_t *base, struct encryption_info_command_64 *cmd) {
    printf("%-20s cmdsize: %-5u cryptoff: %u  cryptsize: %u  (range: %#x-%#x)  cryptid: %u   pad: %u\n",
        "LC_ENCRYPTION_INFO_64", cmd->cmdsize, cmd->cryptoff, cmd->cryptsize, cmd->cryptoff,
        cmd->cryptoff + cmd->cryptsize, cmd->cryptid, cmd->pad);
}
