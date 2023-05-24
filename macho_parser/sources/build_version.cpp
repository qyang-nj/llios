#include <string.h>

#include "utils/utils.h"
#include "argument.h"
#include "build_version.h"

static void get_tool_name(uint32_t tool, char *tool_name);
static void get_platform_name(uint32_t platform, char *platform_name);

void printBuildVersion(const uint8_t *base, const struct build_version_command *build_version_cmd) {
    char platform_name[128];

    get_platform_name(build_version_cmd->platform, platform_name);

    auto minos = formatVersion(build_version_cmd->minos);
    auto sdk = formatVersion(build_version_cmd->sdk);

    printf("%-20s cmdsize: %-6u platform: %s   minos: %s   sdk: %s\n", "LC_BUILD_VERSION",
        build_version_cmd->cmdsize,
        platform_name, minos.c_str(), sdk.c_str());

    if (args.verbosity == 0) {
        return;
    }

    for (int i = 0; i < build_version_cmd->ntools; ++i) {
        char tool_name[128];

        struct build_tool_version *tool_version = (struct build_tool_version *)((uint8_t *)build_version_cmd + sizeof(struct build_version_command) + i * sizeof(struct build_tool_version));

        get_tool_name(tool_version->tool, tool_name);

        auto toolVersionString = formatVersion(tool_version->version);

        printf ("    tool:  %s   version: %s\n", tool_name, toolVersionString.c_str());
    }
}

void printVersionMin(const uint8_t *base, const struct version_min_command *version_min_cmd) {
    const char *cmd_name = NULL;
    switch (version_min_cmd->cmd) {
        case LC_VERSION_MIN_MACOSX: cmd_name = "LC_VERSION_MIN_MACOSX"; break;
        case LC_VERSION_MIN_IPHONEOS: cmd_name = "LC_VERSION_MIN_IPHONEOS"; break;
        case LC_VERSION_MIN_WATCHOS: cmd_name = "LC_VERSION_MIN_WATCHOS"; break;
        case LC_VERSION_MIN_TVOS: cmd_name = "LC_VERSION_MIN_TVOS"; break;
    }

    auto version = formatVersion(version_min_cmd->version);
    auto sdk = formatVersion(version_min_cmd->sdk);

    printf("%-20s cmdsize: %-6u version: %s   sdk: %s\n",
        cmd_name, version_min_cmd->cmdsize,
        version.c_str(), sdk.c_str());
}

static void get_platform_name(uint32_t platform, char *platform_name) {
    switch (platform) {
        case PLATFORM_MACOS: strcpy(platform_name, "MACOS"); break;
        case PLATFORM_IOS: strcpy(platform_name, "IOS"); break;
        case PLATFORM_TVOS: strcpy(platform_name, "TVOS"); break;
        case PLATFORM_WATCHOS: strcpy(platform_name, "WATCHOS"); break;
        case PLATFORM_BRIDGEOS: strcpy(platform_name, "BRIDGEOS"); break;
        case PLATFORM_MACCATALYST: strcpy(platform_name, "MACCATALYST"); break;
        case PLATFORM_IOSSIMULATOR: strcpy(platform_name, "IOSSIMULATOR"); break;
        case PLATFORM_TVOSSIMULATOR: strcpy(platform_name, "TVOSSIMULATOR"); break;
        case PLATFORM_WATCHOSSIMULATOR: strcpy(platform_name, "WATCHOSSIMULATOR"); break;
        case PLATFORM_DRIVERKIT: strcpy(platform_name, "PLATFORM_DRIVERKIT"); break;
        default: strcpy(platform_name, "UNKNOWN"); break;
    }
}

static void get_tool_name(uint32_t tool, char *tool_name) {
    switch (tool) {
        case TOOL_CLANG: strcpy(tool_name, "CLANG"); break;
        case TOOL_SWIFT: strcpy(tool_name, "SWIFT"); break;
        case TOOL_LD: strcpy(tool_name, "LD"); break;
        default: strcpy(tool_name, "UNKNOWN"); break;
    }
}
