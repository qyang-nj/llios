#include <string.h>

#include "argument.h"
#include "build_version.h"

static void get_tool_name(uint32_t tool, char *tool_name);
static void get_platform_name(uint32_t platform, char *platform_name);
static void get_version_string(uint32_t version, char *version_string);

void parse_build_version(void *base, struct build_version_command *build_version_cmd) {
    char platform_name[128];
    char minos_string[128];
    char sdk_string[128];

    get_platform_name(build_version_cmd->platform, platform_name);
    get_version_string(build_version_cmd->minos, minos_string);
    get_version_string(build_version_cmd->sdk, sdk_string);

    printf("%-20s cmdsize: %-6u platform: %s   minos: %s   sdk: %s\n", "LC_BUILD_VERSION",
        build_version_cmd->cmdsize,
        platform_name, minos_string, sdk_string);

    if (args.verbose == 0) {
        return;
    }

    for (int i = 0; i < build_version_cmd->ntools; ++i) {
        char tool_name[128];
        char tool_version_string[128];

        struct build_tool_version *tool_version = (void *)build_version_cmd
            + sizeof(struct build_version_command)
            + i * sizeof(struct build_tool_version);

        get_tool_name(tool_version->tool, tool_name);
        get_version_string(tool_version->version, tool_version_string);

        printf ("    tool:  %s   version: %s\n", tool_name, tool_version_string);
    }
}

void parse_version_min(void *base, struct version_min_command *version_min_cmd) {
    char *cmd_name = NULL;
    switch (version_min_cmd->cmd) {
        case LC_VERSION_MIN_MACOSX: cmd_name = "LC_VERSION_MIN_MACOSX"; break;
        case LC_VERSION_MIN_IPHONEOS: cmd_name = "LC_VERSION_MIN_IPHONEOS"; break;
        case LC_VERSION_MIN_WATCHOS: cmd_name = "LC_VERSION_MIN_WATCHOS"; break;
        case LC_VERSION_MIN_TVOS: cmd_name = "LC_VERSION_MIN_TVOS"; break;
    }

    char version_string[128];
    char sdk_version[128];
    get_version_string(version_min_cmd->version, version_string);
    get_version_string(version_min_cmd->sdk, sdk_version);

    printf("%-20s cmdsize: %-6u version: %s   sdk: %s\n",
        cmd_name, version_min_cmd->cmdsize,
        version_string, sdk_version);
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

static void get_version_string(uint32_t version, char *version_string) {
    int patch = (version & 0xFF);
    int minor = (version & 0xFF00) >> 8;
    int major = (version & 0xFFFF0000) >> 16;

    sprintf(version_string, "%d.%d.%d", major, minor, patch);
}

static void get_tool_name(uint32_t tool, char *tool_name) {
    switch (tool) {
        case TOOL_CLANG: strcpy(tool_name, "CLANG"); break;
        case TOOL_SWIFT: strcpy(tool_name, "SWIFT"); break;
        case TOOL_LD: strcpy(tool_name, "LD"); break;
        default: strcpy(tool_name, "UNKNOWN"); break;
    }
}
