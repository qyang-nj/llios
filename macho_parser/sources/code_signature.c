#include <stdio.h>
#include <string.h>
#include <libkern/OSByteOrder.h>
// /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/kern/cs_blobs.h
#include <Kernel/kern/cs_blobs.h>

#include "code_signature.h"

static void format_blob_name(uint32_t blob_type, char *formatted);

void parse_code_signature(void *base, uint32_t dataoff, uint32_t datasize) {
    CS_SuperBlob *super_blob = base + dataoff;

    printf("    SuperBlob maggic: 0x%x, length: %d, count: %d\n",
        OSSwapInt32(super_blob->magic),
        OSSwapInt32(super_blob->length),
        OSSwapInt32(super_blob->count));

    for (int i = 0; i < OSSwapInt32(super_blob->count); ++i) {
        uint32_t blob_type = OSSwapInt32(super_blob->index[i].type);
        uint32_t blob_offset = OSSwapInt32(super_blob->index[i].offset);

        char blob_name[256];
        format_blob_name(blob_type, blob_name);
        printf("        Blob %d: type: %s, offset: %d\n", i, blob_name, blob_offset);

        if (blob_type == CSSLOT_CODEDIRECTORY) {
            CS_CodeDirectory *code_directory = (void *)super_blob + blob_offset;
            printf("%x\n\n", OSSwapInt32(code_directory->magic));
        } else if (blob_type == CSSLOT_ENTITLEMENTS) {
            CS_GenericBlob *generic_blob = (void *)super_blob + blob_offset;
            printf("%.*s\n\n", OSSwapInt32(generic_blob->length), generic_blob->data);
        }
    }
}

static void format_blob_name(uint32_t blob_type, char *formatted) {
    switch(blob_type) {
        case CSSLOT_CODEDIRECTORY: strcpy(formatted, "CSSLOT_CODEDIRECTORY"); break;
        case CSSLOT_INFOSLOT: strcpy(formatted, "CSSLOT_INFOSLOT"); break;
        case CSSLOT_REQUIREMENTS: strcpy(formatted, "CSSLOT_REQUIREMENTS"); break;
        case CSSLOT_RESOURCEDIR: strcpy(formatted, "CSSLOT_RESOURCEDIR"); break;
        case CSSLOT_APPLICATION: strcpy(formatted, "CSSLOT_APPLICATION"); break;
        case CSSLOT_ENTITLEMENTS: strcpy(formatted, "CSSLOT_ENTITLEMENTS"); break;
        // case CSSLOT_SIGNATURESLOT: strcpy(formatted, "CSSLOT_SIGNATURESLOT"); break;
        default: sprintf(formatted, "UNKNOWN(0x%x)", blob_type);
    }
}
