#include <stdio.h>
#include <string.h>
#include <math.h>

#include <Kernel/kern/cs_blobs.h>    // /Kernel.framework
#include <Security/SecRequirement.h> // Security.framework

#ifdef OPENSSL
#include <openssl/pkcs7.h>
#include <openssl/sha.h>
#endif

extern "C" {
#include "argument.h"
}

#include "utils/utils.h"
#include "code_signature.h"

static void printCodeDirectory(CS_CodeDirectory *codeDirectory);
static void printRequirement(unsigned char *data, int size);
static void printPKCS7(const unsigned char* buffer, size_t size);
static void formatBlobMagic(uint32_t magic, char *formatted, size_t output_size);
static void formatHashType(uint8_t hash_type, char *formatted, size_t output_size);
static std::string cdHash(CS_CodeDirectory *codeDirectory);
static std::string sha1(const unsigned char *data, size_t size);
static std::string sha256(const unsigned char *data, size_t size);

void printCodeSignature(uint8_t *base, uint32_t dataoff, uint32_t datasize) {
    char magic_name[256];

    CS_SuperBlob *super_blob = (CS_SuperBlob *)(base + dataoff);

    // Code signature is always encoded in network byte ordering,
    // so we need to use ntohl to covert byte order from network to host.
    formatBlobMagic(ntohl(super_blob->magic), magic_name, sizeof(magic_name));
    printf("SuperBlob: magic: %s, length: %d, count: %d\n", magic_name, ntohl(super_blob->length), ntohl(super_blob->count));
    for (int i = 0; i < ntohl(super_blob->count); ++i) {
        uint32_t blob_type = ntohl(super_blob->index[i].type);
        uint32_t blob_offset = ntohl(super_blob->index[i].offset);

        CS_GenericBlob *blob = (CS_GenericBlob *)((uint8_t *)super_blob + blob_offset);
        uint32_t magic = ntohl(blob->magic);
        formatBlobMagic(magic, magic_name, sizeof(magic_name));

        printf("  Blob %d: type: %#07x, offset: %d, magic: %s, length: %d", i, blob_type, blob_offset, magic_name, ntohl(blob->length));
        if (blob_type == 0x7 && magic == 0xfade7172) {
            printf("  (likely DER entitlements)");
        }
        printf("\n");

        if (magic == CSMAGIC_CODEDIRECTORY) {
            if (args.show_code_direcotry) {
                CS_CodeDirectory *cd_blob = (CS_CodeDirectory *)((uint8_t *)super_blob + blob_offset);
                printCodeDirectory(cd_blob);
            }
        } else if (magic == CSMAGIC_EMBEDDED_ENTITLEMENTS) {
            if (args.show_entitlement) {
                CS_GenericBlob *ent_blob = (CS_GenericBlob *)((uint8_t *)super_blob + blob_offset);
                printf("%.*s\n\n", ntohl(ent_blob->length), ent_blob->data);
            }
        } else if (magic == CSMAGIC_REQUIREMENTS) {
            if (args.verbosity < 2) { continue; }
            CS_SuperBlob *req_super_blob = (CS_SuperBlob *)((uint8_t *)super_blob + blob_offset);
            for (int j = 0; j < ntohl(req_super_blob->count); ++j) {
                uint32_t req_blob_offset = ntohl(req_super_blob->index[j].offset);
                CS_GenericBlob *req_blob = (CS_GenericBlob *)((uint8_t *)req_super_blob + req_blob_offset);

                printf("    Requirement[%d]: offset: %d, length: %d\n", j, req_blob_offset, ntohl(req_blob->length));
                printRequirement((unsigned char *)req_blob, ntohl(req_blob->length));
            }
            printf("\n");
        } else if (magic == CSMAGIC_BLOBWRAPPER) {
            if (args.show_blob_wrapper) {
                CS_GenericBlob *blob_wrapper = (CS_GenericBlob *)((uint8_t *)super_blob + blob_offset);
                printPKCS7((const unsigned char *)blob_wrapper->data, ntohl(blob_wrapper->length));
            }
        }
    }
}

static void printRequirement(unsigned char *data, int size) {
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

static void printCodeDirectory(CS_CodeDirectory *codeDirectory) {
    uint32_t hash_offset = ntohl(codeDirectory->hashOffset);
    uint8_t hash_size = codeDirectory->hashSize;
    uint32_t special_slot_size = ntohl(codeDirectory->nSpecialSlots);
    uint32_t slot_size = ntohl(codeDirectory->nCodeSlots);
    uint32_t identity_offset = ntohl(codeDirectory->identOffset);

    char hash_type[32];
    formatHashType(codeDirectory->hashType, hash_type, sizeof(hash_type));

    printf("    version      : %#x\n", ntohl(codeDirectory->version));
    printf("    flags        : %#x\n", ntohl(codeDirectory->flags));
    printf("    hashOffset   : %d\n", hash_offset);
    printf("    identOffset  : %d\n", identity_offset);
    printf("    nSpecialSlots: %d\n", special_slot_size);
    printf("    nCodeSlots   : %d\n", ntohl(codeDirectory->nCodeSlots));
    printf("    codeLimit    : %d\n", ntohl(codeDirectory->codeLimit));
    printf("    hashSize     : %d\n", hash_size);
    printf("    hashType     : %s\n", hash_type);
    printf("    platform     : %d\n", (codeDirectory->platform));
    printf("    pageSize     : %d\n", (int)pow(2, codeDirectory->pageSize));
    printf("    identity     : %s\n", (char *)codeDirectory + identity_offset);

    auto cdhash = cdHash(codeDirectory);
    printf("    CDHash       : %s\n", cdhash.c_str());
    printf("\n");

    uint8_t *hash_base = (uint8_t *)codeDirectory + hash_offset;
    for (int i = special_slot_size; i > 0; --i) {
        auto hash = formatBufferToHex(hash_base - i * hash_size, hash_size);
        printf("    Slot[%3d] : %s\n", -i, hash.c_str());
    }

    int max_number = args.no_truncate ? slot_size : (slot_size > 10 ? 10 : slot_size);

    for (int i = 0; i < max_number; ++i) {
        auto hash = formatBufferToHex(hash_base + i * hash_size, hash_size);
        printf("    Slot[%3d] : %s\n", i, hash.c_str());
    }

    if (!args.no_truncate && slot_size > 10) {
        printf("        ... %d more ...\n", slot_size - 10);
    }
    printf("\n");
}

#ifdef OPENSSL
static void printPKCS7(const unsigned char *data, size_t size) {
    PKCS7* pkcs7 = d2i_PKCS7(NULL, &data, size);
    assert(pkcs7 != NULL);

    BIO *bio = BIO_new(BIO_s_file());
    BIO_set_fp(bio, stdout, BIO_NOCLOSE);
    PKCS7_print_ctx(bio, pkcs7, 4, NULL);
    BIO_free(bio);
}

static std::string sha256(const unsigned char *data, size_t size) {
    unsigned char *result = SHA256(data, size, NULL);
    return formatBufferToHex(result, SHA256_DIGEST_LENGTH);
}

static std::string sha1(const unsigned char *data, size_t size) {
    unsigned char *result = SHA1(data, size, NULL);
    return formatBufferToHex(result, SHA_DIGEST_LENGTH);
}

#else

static void printPKCS7(const unsigned char *data, size_t size) {
    puts("    Info: To show detailed PKCS7 information, use 'build.sh --openssl' and run again.");
}

static std::string sha256(const unsigned char *data, size_t size) {
    return "Unavailable. Use 'build.sh --openssl' and run again.";
}

static std::string sha1(const unsigned char *data, size_t size) {
    return "Unavailable. Use 'build.sh --openssl' and run again.";
}

#endif

static std::string cdHash(CS_CodeDirectory *codeDirectory) {
    switch(codeDirectory->hashType) {
        case CS_HASHTYPE_SHA1:
            return sha1((unsigned char *)codeDirectory, ntohl(codeDirectory->length));
        case CS_HASHTYPE_SHA256:
        case CS_HASHTYPE_SHA256_TRUNCATED:
            return sha256((unsigned char *)codeDirectory, ntohl(codeDirectory->length));
        default:
            return "Unsupported hash type.";
    }
}

static void formatBlobMagic(uint32_t magic, char *formatted, size_t output_size) {
    switch(magic) {
        case CSMAGIC_REQUIREMENT: strcpy(formatted, "CSMAGIC_REQUIREMENT"); break;
        case CSMAGIC_REQUIREMENTS: strcpy(formatted, "CSMAGIC_REQUIREMENTS"); break;
        case CSMAGIC_CODEDIRECTORY: strcpy(formatted, "CSMAGIC_CODEDIRECTORY"); break;
        case CSMAGIC_EMBEDDED_SIGNATURE: strcpy(formatted, "CSMAGIC_EMBEDDED_SIGNATURE"); break;
        case CSMAGIC_EMBEDDED_SIGNATURE_OLD: strcpy(formatted, "CSMAGIC_EMBEDDED_SIGNATURE_OLD"); break;
        case CSMAGIC_EMBEDDED_ENTITLEMENTS: strcpy(formatted, "CSMAGIC_EMBEDDED_ENTITLEMENTS"); break;
        case CSMAGIC_DETACHED_SIGNATURE: strcpy(formatted, "CSMAGIC_DETACHED_SIGNATURE"); break;
        case CSMAGIC_BLOBWRAPPER: strcpy(formatted, "CSMAGIC_BLOBWRAPPER"); break;
        default: snprintf(formatted, output_size, "%#08x", magic);
    }
}

static void formatHashType(uint8_t hash_type, char *formatted, size_t output_size) {
    switch(hash_type) {
        case CS_HASHTYPE_SHA1: strcpy(formatted, "SHA1"); break;
        case CS_HASHTYPE_SHA256: strcpy(formatted, "SHA256"); break;
        case CS_HASHTYPE_SHA256_TRUNCATED: strcpy(formatted, "SHA256_TRUNCATED"); break;
        case CS_HASHTYPE_SHA384: strcpy(formatted, "SHA384"); break;
        default: snprintf(formatted, output_size, "UNKNOWN(%#08x)", hash_type);
    }
}
