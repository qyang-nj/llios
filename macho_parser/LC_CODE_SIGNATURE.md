# LC_CODE_SIGNATURE
After code signing, the `LC_CODE_SIGNATURE` load command will be appended to the mach-o binary. Since code signing is the last step of the building (you can't change anything after signing), this load command is always the last one and is always at the end of a binary.

`LC_CODE_SIGNATURE` is inside `__LINKEDIT` segment and uses the generic `linkedit_data_command`, which merely specifies an area (offset and size) in the file. The actual format is defined in `cs_blobs.h` ([kern/cs_blobs.h](../apple_open_source/xnu/osfmk/kern/cs_blobs.h)).

The full parsing logic can be found at `code_signature.c` (macho_parser/sources/code_signature.c)

## Super Blob
The code signature begins with a super blob, which specifies a list of other blobs. All the blobs have a magic number, which define the type. The most common super blob magic of an iOS app is `CSMAGIC_EMBEDDED_SIGNATURE`, which indicates the signature is embedded in the app binary.
``` c
typedef struct __SC_SuperBlob {
    uint32_t magic;                         /* magic number */
    uint32_t length;                        /* total length of SuperBlob */
    uint32_t count;                         /* number of index entries following */
    CS_BlobIndex index[];                   /* (count) entries */
    /* followed by Blobs in no particular order as indicated by offsets in index */
} CS_SuperBlob
```

## Code Directory Blob
An extremely simplified version of code signing: hash the entire app and encrypt the hash value with your private key. Later, the validator uses your public key to decrypt the hash and compares it with the hash value calculated from the app. If anything is tampered, the two hashes won't be the same.

The code directory blob is mostly about hashes. To calculate the hash of something, the entire thing needs to be loaded into memory. This is not ideal if the app is very large. Brilliantly, code signing actually calculates the hash of each page and the final hash is calculated from those hashes.

``` c
typedef struct __CodeDirectory {
    uint32_t magic;                         /* magic number (CSMAGIC_CODEDIRECTORY) */
    uint32_t length;                        /* total length of CodeDirectory blob */
    uint32_t version;                       /* compatibility version */
    uint32_t flags;                         /* setup and mode flags */
    uint32_t hashOffset;                    /* offset of hash slot element at index zero */
    uint32_t identOffset;                   /* offset of identifier string */
    uint32_t nSpecialSlots;                 /* number of special hash slots */
    uint32_t nCodeSlots;                    /* number of ordinary (code) hash slots */
    uint32_t codeLimit;                     /* limit to main image signature range */
    uint8_t hashSize;                       /* size of each hash in bytes */
    uint8_t hashType;                       /* type of hash (cdHashType* constants) */
    uint8_t platform;                       /* platform identifier; zero if not platform binary */
    uint8_t pageSize;                       /* log2(page size in bytes); 0 => infinite */
    uint32_t spare2;                        /* unused (must be zero) */
    ...
    /* followed by dynamic content as located by offset fields above */
} CS_CodeDirectory
```

### Slots
A slot stores a hash value of one page. `hashOffset` specifies where the first slot is and `nCodeSlots` specifies the number of slots. We can use `codesign` to examine the hash of every page.
```
$ codesign -d -vvvvvv Airbnb.app/Airbnb
...
Page size=4096
    -7=cccc6824b63ba81e6ea128cb825b14e61ee4481ca75ca5ae7988cf392208ab6a
    -6=0000000000000000000000000000000000000000000000000000000000000000
    -5=630d26d5b08cfffa87471ffa3ed537d31f7df33f37d834ca4979d9e91c36fe94
    -4=0000000000000000000000000000000000000000000000000000000000000000
    -3=a9f055b782361af718a647a0ad2018f6c6e619befd99ef1ca8ffabf27f064cdd
    -2=921882ead6cd77c0987eaaf9add0d61421c5489b3edb33b777712ca0f1aae1e7
    -1=479d89aa91b4213cd7e813d6a56c3e6da5d23cc44c2c52481451829fde42abcc
     0=7734a7ca7e3bd08d9d09bb4ec6efac157a4b7227a161e5087c04d5a154c63cd4
     1=cbf08a372b3e08abdc03d61f29160e4b906487e11be486f44309c88da7fbb6e8
     2=a08f433e71884f85538d8736fad2de40149d31b65b797155f90860cfbc84ae66
...
```

These hash algorithm is SHA256. We can run the hash by ourself to verify it.
```
$ dd if=Airbnb.app/Airbnb of=page1.bin bs=$(pagesize) count=1 skip=0
$ openssl sha256 page1.bin
SHA256(page1.bin)= 7734a7ca7e3bd08d9d09bb4ec6efac157a4b7227a161e5087c04d5a154c63cd4
```
We can tell the hash of the first page of the binary is the same as value stored in `slot[0]`.

### Special Slots
You may wonder what those negative slots (-1 to -7) from above snippet are. They are special slots.

The app binary isn't the only thing in the app bundle. There are Info.plist and lots of other files, called resources. Each special slot stores the hash of specific thing.
> -1: Hash of bundle Info.plist
> -2: Hash of embeded code signing requirements (described later)
> -3: Hash of `_CodeSignature/CodeResources`
> -4: App specific hash (usually not used)
> -5: Hash of entitlement embedded in the code signature (described later)

A little more on `_CodeSignature/CodeResources`, it's a plist text file, so you can view the content in a text editor. The file stores the hash of each resource file in the app bundle.

``` bash
# The hash of Info.plist is in slot[-1]
$ openssl sha256 Airbnb.app/Info.plist
SHA256(Airbnb.app/Info.plist)= 479d89aa91b4213cd7e813d6a56c3e6da5d23cc44c2c52481451829fde42abcc

# The hash of _CodeSignature/CodeResources is in slot[-3]
$ openssl sha256 Airbnb.app/_CodeSignature/CodeResources
SHA256(Airbnb.app/_CodeSignature/CodeResources)= a9f055b782361af718a647a0ad2018f6c6e619befd99ef1ca8ffabf27f064cdd
```

## Requirement Blob
Apple's code signing is more than just hashing. We can specify other requirements, like what the app id is and what certificate is required. Apple designed [code signing requirement language](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/RequirementLang/RequirementLang.html)
```
$ codesign -d -r- Airbnb.app/Airbnb
designated => identifier "com.airbnb.app" and anchor apple generic and certificate leaf[subject.CN] = "iPhone Distribution: Airbnb, Inc. (xxxxxxxxxx)" and certificate 1[field.1.2.840.113635.100.6.2.1] /* exists */
```

## Entitlement(s) Blob
The entitlements are capabilities encoded in a plist file.

one blob, multiple blobs

## Signature Blob
The signature, also known as Blob Wrapper, is where all the cryptographic stuff is, including certificates, signer info and more. This blob
