#include <string.h>

#include "utils/utils.h"
#include "argument.h"

static std::string formatToolName(uint32_t tool) ;
static std::string formatPlatformName(uint32_t platform);

void printBuildVersion(const uint8_t *base, const struct build_version_command *buildVersionCmd) {
    auto platformName = formatPlatformName(buildVersionCmd->platform);
    auto minos = formatVersion(buildVersionCmd->minos);
    auto sdk = formatVersion(buildVersionCmd->sdk);

    printf("%-20s cmdsize: %-6u platform: %s   minos: %s   sdk: %s\n", "LC_BUILD_VERSION",
        buildVersionCmd->cmdsize,
        platformName.c_str(), minos.c_str(), sdk.c_str());

    if (args.verbosity == 0) {
        return;
    }

    for (int i = 0; i < buildVersionCmd->ntools; ++i) {
        struct build_tool_version *tool_version = (struct build_tool_version *)((uint8_t *)buildVersionCmd + sizeof(struct build_version_command) + i * sizeof(struct build_tool_version));

        auto toolName = formatToolName(tool_version->tool);
        auto toolVersionString = formatVersion(tool_version->version);

        printf ("    tool:  %s   version: %s\n", toolName.c_str(), toolVersionString.c_str());
    }
}

void printVersionMin(const uint8_t *base, const struct version_min_command *versionMinCmd) {
    const char *cmd_name = NULL;
    switch (versionMinCmd->cmd) {
        case LC_VERSION_MIN_MACOSX: cmd_name = "LC_VERSION_MIN_MACOSX"; break;
        case LC_VERSION_MIN_IPHONEOS: cmd_name = "LC_VERSION_MIN_IPHONEOS"; break;
        case LC_VERSION_MIN_WATCHOS: cmd_name = "LC_VERSION_MIN_WATCHOS"; break;
        case LC_VERSION_MIN_TVOS: cmd_name = "LC_VERSION_MIN_TVOS"; break;
    }

    auto version = formatVersion(versionMinCmd->version);
    auto sdk = formatVersion(versionMinCmd->sdk);

    printf("%-20s cmdsize: %-6u version: %s   sdk: %s\n",
        cmd_name, versionMinCmd->cmdsize,
        version.c_str(), sdk.c_str());
}

static std::string formatPlatformName(uint32_t platform) {
    switch (platform) {
        case PLATFORM_MACOS: return "MACOS";
        case PLATFORM_IOS: return"IOS";
        case PLATFORM_TVOS: return"TVOS";
        case PLATFORM_WATCHOS: return"WATCHOS";
        case PLATFORM_BRIDGEOS: return"BRIDGEOS";
        case PLATFORM_MACCATALYST: return"MACCATALYST";
        case PLATFORM_IOSSIMULATOR: return"IOSSIMULATOR";
        case PLATFORM_TVOSSIMULATOR: return"TVOSSIMULATOR";
        case PLATFORM_WATCHOSSIMULATOR: return"WATCHOSSIMULATOR";
        case PLATFORM_DRIVERKIT: return"PLATFORM_DRIVERKIT";
        default: return"UNKNOWN";
    }
}

static std::string formatToolName(uint32_t tool) {
    switch (tool) {
        case TOOL_CLANG: return "CLANG";
        case TOOL_SWIFT: return "SWIFT";
        case TOOL_LD: return "LD";
        default: return "UNKNOWN";
    }
}
