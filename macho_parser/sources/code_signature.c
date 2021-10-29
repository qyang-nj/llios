#include <stdio.h>
#include <string.h>
#include <math.h>
#include <libkern/OSByteOrder.h>
// /Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/System/Library/Frameworks/Kernel.framework/Versions/A/Headers/kern/cs_blobs.h
#include <Kernel/kern/cs_blobs.h>

#include "util.h"
#include "argument.h"
#include "code_signature.h"

static void print_code_directory(CS_CodeDirectory *code_directory);
static void format_blob_name(uint32_t blob_type, char *formatted);
static void format_blob_magic(uint32_t magic, char *formated);

void parse_code_signature(void *base, uint32_t dataoff, uint32_t datasize) {
    char magic_name[256];

    CS_SuperBlob *super_blob = base + dataoff;
    format_blob_magic(OSSwapInt32(super_blob->magic), magic_name);

    printf("SuperBlob | maggic: %s, length: %d, count: %d\n",
        magic_name,
        OSSwapInt32(super_blob->length),
        OSSwapInt32(super_blob->count));

    for (int i = 0; i < OSSwapInt32(super_blob->count); ++i) {
        uint32_t blob_type = OSSwapInt32(super_blob->index[i].type);
        uint32_t blob_offset = OSSwapInt32(super_blob->index[i].offset);

        uint32_t magic = OSSwapInt32(*(uint32_t *)((void *)super_blob + blob_offset));
        format_blob_magic(magic, magic_name);

        printf("  Blob %d | type: %#07x, offset: %-7d, magic: %s\n", i, blob_type, blob_offset, magic_name);

        if (magic == CSMAGIC_CODEDIRECTORY) {
            CS_CodeDirectory *code_directory = (void *)super_blob + blob_offset;
            print_code_directory(code_directory);
        } else if (magic == CSMAGIC_EMBEDDED_ENTITLEMENTS) {
            CS_GenericBlob *generic_blob = (void *)super_blob + blob_offset;
            // printf("%.*s\n\n", OSSwapInt32(generic_blob->length), generic_blob->data);
        }
    }
}

static void print_code_directory(CS_CodeDirectory *code_directory) {
    uint32_t hash_offset = OSSwapInt32(code_directory->hashOffset);
    uint8_t hash_size = code_directory->hashSize;
    uint32_t special_slot_size = OSSwapInt32(code_directory->nSpecialSlots);
    uint32_t slot_size = OSSwapInt32(code_directory->nCodeSlots);
    uint32_t identity_offset = OSSwapInt32(code_directory->identOffset);

    printf("    magic        : %#x\n", OSSwapInt32(code_directory->magic));
    printf("    length       : %d\n", OSSwapInt32(code_directory->length));
    printf("    version      : %#x\n", OSSwapInt32(code_directory->version));
    printf("    flags        : %#x\n", OSSwapInt32(code_directory->flags));
    printf("    hashOffset   : %d\n", hash_offset);
    printf("    identOffset  : %d\n", identity_offset);
    printf("    nSpecialSlots: %d\n", special_slot_size);
    printf("    nCodeSlots   : %d\n", OSSwapInt32(code_directory->nCodeSlots));
    printf("    codeLimit    : %d\n", OSSwapInt32(code_directory->codeLimit));
    printf("    hashSize     : %d\n", hash_size);
    printf("    hashType     : %d\n", (code_directory->hashType));
    printf("    platform     : %d\n", (code_directory->platform));
    printf("    pageSize     : %d\n", (int)pow(2, code_directory->pageSize));
    printf("    identity     : %s\n", (char *)code_directory + identity_offset);
    printf("\n");

    uint8_t *hash_base = (void *)code_directory + hash_offset;
    char hash[256];
    for (int i = special_slot_size; i > 0; --i) {
        bzero(hash, sizeof(hash));
        format_hex(hash_base - i * hash_size, hash_size, hash);
        printf("    Slot[%3d] : %s\n", -i, hash);
    }

    int max_number = args.verbose == 1 ? (slot_size > 10 ? 10 : slot_size) : slot_size;

    for (int i = 0; i < max_number; ++i) {
        bzero(hash, sizeof(hash));
        format_hex(hash_base + i * hash_size, hash_size, hash);
        printf("    Slot[%3d] : %s\n", i, hash);
    }

    if (args.verbose == 1 && slot_size > 10) {
        printf("        ... %d more ...\n", slot_size - 10);
    }
    printf("\n");
}

static void format_blob_name(uint32_t blob_type, char *formatted) {
    switch(blob_type) {
        case CSSLOT_CODEDIRECTORY: strcpy(formatted, "CSSLOT_CODEDIRECTORY"); break;
        case CSSLOT_INFOSLOT: strcpy(formatted, "CSSLOT_INFOSLOT"); break;
        case CSSLOT_REQUIREMENTS: strcpy(formatted, "CSSLOT_REQUIREMENTS"); break;
        case CSSLOT_RESOURCEDIR: strcpy(formatted, "CSSLOT_RESOURCEDIR"); break;
        case CSSLOT_APPLICATION: strcpy(formatted, "CSSLOT_APPLICATION"); break;
        case CSSLOT_ENTITLEMENTS: strcpy(formatted, "CSSLOT_ENTITLEMENTS"); break;
        case CSSLOT_SIGNATURESLOT: strcpy(formatted, "CSSLOT_SIGNATURESLOT"); break;
        default: sprintf(formatted, "UNKNOWN(0x%x)", blob_type);
    }
}

static void format_blob_magic(uint32_t magic, char *formatted) {
    switch(magic) {
        case CSMAGIC_REQUIREMENT: strcpy(formatted, "CSMAGIC_REQUIREMENT"); break;
        case CSMAGIC_REQUIREMENTS: strcpy(formatted, "CSMAGIC_REQUIREMENTS"); break;
        case CSMAGIC_CODEDIRECTORY: strcpy(formatted, "CSMAGIC_CODEDIRECTORY"); break;
        case CSMAGIC_EMBEDDED_SIGNATURE: strcpy(formatted, "CSMAGIC_EMBEDDED_SIGNATURE"); break;
        case CSMAGIC_EMBEDDED_SIGNATURE_OLD: strcpy(formatted, "CSMAGIC_EMBEDDED_SIGNATURE_OLD"); break;
        case CSMAGIC_EMBEDDED_ENTITLEMENTS: strcpy(formatted, "CSMAGIC_EMBEDDED_ENTITLEMENTS"); break;
        case CSMAGIC_DETACHED_SIGNATURE: strcpy(formatted, "CSMAGIC_DETACHED_SIGNATURE"); break;
        case CSMAGIC_BLOBWRAPPER: strcpy(formatted, "CSMAGIC_BLOBWRAPPER"); break;
        default: sprintf(formatted, "UNKNOWN(%#08x)", magic);
    }
}
