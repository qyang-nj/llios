#ifndef ENCRYPTION_INFO_H
#define ENCRYPTION_INFO_H

#include <mach-o/loader.h>


void printEncryptionInfo(uint8_t *base, struct encryption_info_command_64 *cmd);

#endif // ENCRYPTION_INFO_H
