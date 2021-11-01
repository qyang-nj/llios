#include <stdio.h>
#include <string.h>
#include <math.h>

#include <Kernel/kern/cs_blobs.h>    // /Kernel.framework
#include <Security/SecRequirement.h> // Security.framework

#ifdef OPENSSL
#include <openssl/pkcs7.h>
#endif

#include "util.h"
#include "argument.h"
#include "code_signature.h"

static void print_code_directory(CS_CodeDirectory *code_directory);
static void print_requirement(unsigned char *data, int size);
static void print_pkcs7(const unsigned char* buffer, size_t size);
static void format_blob_magic(uint32_t magic, char *formated);

void parse_code_signature(void *base, uint32_t dataoff, uint32_t datasize) {
    char magic_name[256];

    CS_SuperBlob *super_blob = base + dataoff;

    // Code signature is always encoded in network byte ordering,
    // so we need to use ntohl to covert byte order from network to host.
    format_blob_magic(ntohl(super_blob->magic), magic_name);
    printf("SuperBlob: magic: %s, length: %d, count: %d\n", magic_name, ntohl(super_blob->length), ntohl(super_blob->count));

    for (int i = 0; i < ntohl(super_blob->count); ++i) {
        uint32_t blob_type = ntohl(super_blob->index[i].type);
        uint32_t blob_offset = ntohl(super_blob->index[i].offset);

        uint32_t magic = ntohl(*(uint32_t *)((void *)super_blob + blob_offset));
        format_blob_magic(magic, magic_name);

        printf("  Blob %d: type: %#07x, offset: %-7d, magic: %s\n", i, blob_type, blob_offset, magic_name);

        if (args.verbose <= 1) {
            continue;
        }

        if (magic == CSMAGIC_CODEDIRECTORY) {
            CS_CodeDirectory *code_directory = (void *)super_blob + blob_offset;
            print_code_directory(code_directory);
        } else if (magic == CSMAGIC_EMBEDDED_ENTITLEMENTS) {
            CS_GenericBlob *generic_blob = (void *)super_blob + blob_offset;
            printf("%.*s\n\n", ntohl(generic_blob->length), generic_blob->data);
        } else if (magic == CSMAGIC_REQUIREMENTS) {
            CS_SuperBlob *req_super_blob = (void *)super_blob + blob_offset;
            for (int j = 0; j < ntohl(req_super_blob->count); ++j) {
                uint32_t req_blob_offset = ntohl(req_super_blob->index[j].offset);
                CS_GenericBlob *req_blob = (void *)req_super_blob + req_blob_offset;

                printf("    Requirement[%d]: offset: %d, length: %d\n", j, req_blob_offset, ntohl(req_blob->length));
                print_requirement((unsigned char *)req_blob, ntohl(req_blob->length));
            }
            printf("\n");
        } else if (magic == CSMAGIC_BLOBWRAPPER) {
            CS_GenericBlob *generic_blob = (void *)super_blob + blob_offset;
            print_pkcs7((const unsigned char *)generic_blob->data, ntohl(generic_blob->length));
        }
    }
}

static void print_requirement(unsigned char *data, int size) {
    SecRequirementRef requirement;
    CFStringRef text;
    int err_code;

    CFDataRef requirement_data = CFDataCreate(kCFAllocatorDefault, data, size);

    err_code = SecRequirementCreateWithData(requirement_data, kSecCSDefaultFlags, &requirement);
    if (errSecSuccess != err_code) {
        printf("An error(%d) occurs while parsing requirement binary.\n", err_code);
        CFRelease(requirement_data);
        return;
    }

    err_code = SecRequirementCopyString(requirement, kSecCSDefaultFlags, &text);
    if (errSecSuccess != err_code) {
        printf("An error(%d) occurs while de-compiling requirement.\n", err_code);
        CFRelease(requirement_data);
        return;
    }

    printf("      %s\n", CFStringGetCStringPtr(text, kCFStringEncodingUTF8));

    CFRelease(requirement_data);
}

static void print_code_directory(CS_CodeDirectory *code_directory) {
    uint32_t hash_offset = ntohl(code_directory->hashOffset);
    uint8_t hash_size = code_directory->hashSize;
    uint32_t special_slot_size = ntohl(code_directory->nSpecialSlots);
    uint32_t slot_size = ntohl(code_directory->nCodeSlots);
    uint32_t identity_offset = ntohl(code_directory->identOffset);

    printf("    length       : %d\n", ntohl(code_directory->length));
    printf("    version      : %#x\n", ntohl(code_directory->version));
    printf("    flags        : %#x\n", ntohl(code_directory->flags));
    printf("    hashOffset   : %d\n", hash_offset);
    printf("    identOffset  : %d\n", identity_offset);
    printf("    nSpecialSlots: %d\n", special_slot_size);
    printf("    nCodeSlots   : %d\n", ntohl(code_directory->nCodeSlots));
    printf("    codeLimit    : %d\n", ntohl(code_directory->codeLimit));
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

    int max_number = args.verbose > 2 ? slot_size : (slot_size > 10 ? 10 : slot_size);

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

#ifdef OPENSSL
static void print_pkcs7(const unsigned char *data, size_t size) {
    PKCS7* pkcs7 = d2i_PKCS7(NULL, &data, size);
    assert(pkcs7 != NULL);

    BIO *bio = BIO_new(BIO_s_file());
    BIO_set_fp(bio, stdout, BIO_NOCLOSE);
    PKCS7_print_ctx(bio, pkcs7, 4, NULL);
    BIO_free(bio);
}
#else
static void print_pkcs7(const unsigned char *data, size_t size) {
    puts("    Info: To show detailed PKCS7 information, use 'build.sh --openssl' and run again.");
}
#endif

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
