#ifndef BUILD_VERSION_H
#define BUILD_VERSION_H

#include <stdio.h>
#include <mach-o/loader.h>

// parse LC_BUILD_VERSION
void printBuildVersion(const uint8_t *base, const struct build_version_command *build_version_cmd);

// parse LC_VERSION_MIN_MACOSX, LC_VERSION_MIN_IPHONEOS, LC_VERSION_MIN_WATCHOS, LC_VERSION_MIN_TVOS
void printVersionMin(const uint8_t *base, const struct version_min_command *version_min_cmd);

#endif /* BUILD_VERSION_H */
