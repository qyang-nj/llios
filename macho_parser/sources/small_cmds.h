#ifndef SMALL_CMDS_H
#define SMALL_CMDS_H

#include <mach-o/loader.h>

void printDyLinker(void *base, struct dylinker_command *);
void printEntryPoint(void *base, struct entry_point_command *);
void printLinkerOption(void *base, struct linker_option_command *);
void printRpath(void *base, struct rpath_command *);
void printUUID(void *base, struct uuid_command *cmd);
void printSourceVersion(void *base, struct source_version_command *cmd);
void printThread(uint8_t *base, struct thread_command *cmd);

#endif /* SMALL_CMDS_H */
