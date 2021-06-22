#ifndef BUILD_VERSION_H
#define BUILD_VERSION_H

#include <stdio.h>
#include <mach-o/loader.h>

void parse_build_version(FILE *fptr, struct build_version_command *build_version_cmd);

void parse_version_min(FILE *fptr, struct version_min_command *version_min_cmd);

#endif /* BUILD_VERSION_H */
