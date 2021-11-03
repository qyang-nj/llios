# LC_CODE_SIGNATURE
Code signing is an essential part of build process. It is required for installing and distributing an iOS app. After signing, the `LC_CODE_SIGNATURE` load command is appended to the mach-o binary. Since code signing is the last step of the build process (you shouldn't alter anything after signing), this load command is always at the end of a binary.

`LC_CODE_SIGNATURE` lives inside `__LINKEDIT` segment and uses the generic `linkedit_data_command`, which merely specifies an area (offset and size) in the file. The actual format is defined in `cs_blobs.h` ([kern/cs_blobs.h](../../apple_open_source/xnu/osfmk/kern/cs_blobs.h)).

I have implemented the parsing logic in my [macho parser](..). Check out `code_signature.c` ([macho_parser/sources/code_signature.c](../sources/code_signature.c)) for the full code.
```
parser -c LC_CODE_SIGNATURE -vvv {app_binary}
```

## Super Blob
The code signature begins with a super blob, which specifies a list of other blobs. All the blobs have a magic number, which basically means the type of the blob. The most common super blob magic for an iOS app is `CSMAGIC_EMBEDDED_SIGNATURE`, which indicates the signature is embedded in the app binary.
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

The code directory blob is mostly about hashes. To calculate the hash of something, the entire content needs to be loaded into memory. This is not practical to load all the code and resources at launch. Brilliantly, code signing actually calculates the hash of each page and the final hash is derived from those hashes.

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

### Code Slots
A code slot stores a hash value of one page. `hashOffset` specifies where the first slot is and `nCodeSlots` specifies the number of slots. We can use `codesign` to examine the hash of every page.
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

The hash algorithm (`hashType`) is also recorded in this blob. Usually it's SHA256. We can run the hash by ourself to verify it.
``` bash
# Take out the first page of an app binary
$ dd if=Airbnb.app/Airbnb of=page1.bin bs=$(pagesize) count=1 skip=0
$ openssl sha256 page1.bin
SHA256(page1.bin)= 7734a7ca7e3bd08d9d09bb4ec6efac157a4b7227a161e5087c04d5a154c63cd4
```
We can tell that the hash of the first page is the same as value stored in `slot[0]`.

### Special Slots
You may wonder what those negative slots (-1 to -7) are. They are special slots.

The app binary isn't the only thing in the app bundle. There are Info.plist and lots of other files, a.k.a resources. Each special slot stores the hash of specific thing.

> -1: Hash of bundle Info.plist\
> -2: Hash of embedded code signing requirements (described later)\
> -3: Hash of `_CodeSignature/CodeResources`\
> -4: App specific hash (usually not used)\
> -5: Hash of entitlement embedded in the code signature (described later)\
> -6/-7: They're not defined in the recent open source cs_blobs.h. My guess is that it's related to the new macOS 12.0 entitlement format change (see below).

A little more on `_CodeSignature/CodeResources`, it's a plist xml file, so you can view the content in any text editor. The file stores the hash of each resource file in the app bundle.
```
$ cat Airbnb.app/_CodeSignature/CodeResources
    ...
    <key>embedded.mobileprovision</key>
    <dict>
        <key>hash2</key>
        <data>W+OyYSNn1jS5CdTiNkwHFrZ6BH9EfdmaSsBicM+mjI8=</data>
    </dict>
    ...
```
Same as code slots, we can verify special slots by running sha256.

``` bash
# The hash of Info.plist is in slot[-1]
$ openssl sha256 Airbnb.app/Info.plist
SHA256(Airbnb.app/Info.plist)= 479d89aa91b4213cd7e813d6a56c3e6da5d23cc44c2c52481451829fde42abcc

# The hash of _CodeSignature/CodeResources is in slot[-3]
$ openssl sha256 Airbnb.app/_CodeSignature/CodeResources
SHA256(Airbnb.app/_CodeSignature/CodeResources)= a9f055b782361af718a647a0ad2018f6c6e619befd99ef1ca8ffabf27f064cdd
```

### CDHash
The hash of the entire blob, including `CS_CodeDirectory` struct, slots and special slots, is called Code Directory Hash (CDHash). This is the ultimate hash of the whole app bundle.

Each page and resource are hashed into slots. Slots are hashed to CDHash. CDHash is encrypted by a private key and the corresponding public key is certified by Apple. Through the chain-of-trust, every bit in the app is eventually verified by Apple.

## Requirement Blob
Apple's code signing is more than just hashing. We can enforce other requirements, like what the app id is and what certificate is required. The requirements are specified by [code signing requirement language](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/RequirementLang/RequirementLang.html) and encoded with op codes defined in `requirement.h` ([libsecurity_codesigning/lib/requirement.h](../../apple_open_source/libsecurity_codesigning/lib/requirement.h)). The hash of this blob is stored in `slot[-2]`.

The `Security.framework` provides `SecRequirementCopyString` method to decompile the op codes to a human readable string. We can also use `codesign` command to output the requirements.

```
$ codesign -d -r- Airbnb.app/Airbnb
designated => identifier "com.airbnb.app" and anchor apple generic and certificate leaf[subject.CN] = "iPhone Distribution: Airbnb, Inc. (xxxxxxxxxx)" and certificate 1[field.1.2.840.113635.100.6.2.1] /* exists */
```

From above, the signing requirements of Airbnb app are:
* `identifier "com.airbnb.app"` - The signing identifier is exactly "com.airbnb.app".
* `anchor apple generic` - The certificate chain must lead to an Apple root.
* `certificate leaf[subject.CN`]` = "iPhone Distribution: Airbnb, Inc. (xxxxxxxxxx)" - The leaf (signing) certificate must be Airbnb Distribution.
* `certificate 1[field.1.2.840.113635.100.6.2.1]` - The certificate that issues the leaf certificate must have filed `1.2.840.113635.100.6.2.1`, which means it has to be Apple Worldwide Developer Relations Certification Authority.

These are default requirements. Most iOS app should have similar ones.

## Entitlement(s) Blob
The entitlements are capabilities encoded in a plist file. The entire content is embedded in entitlements blob in plain text.

There are two types of entitlement blob: single entitlement blob (`CSMAGIC_REQUIREMENT`) and multiple entitlement blob (`CSMAGIC_REQUIREMENTS`). The multiple entitlements blob is actually a super blob.

Since it's just plain ASCII text, parsing this blob is simply to print it out.

``` bash
# Show the embedded entitlement
$ codesign -d --entitlements - Airbnb.app/Airbnb
```

### New on macOS 12.0 (Monterey)
Starting with macOS 12.0, code signing uses `--generate-entitlement-der` by default, which "converts the supplied entitlements XML data to DER and embed the entitlements as both XML and DER in the signature."

More info is on the Apple doc, [Using the Latest Code Signature Format](https://developer.apple.com/documentation/xcode/using-the-latest-code-signature-format).

## Signature Blob
The signature, also called Blob Wrapper, is where all the cryptographic stuff is. This blob uses Cryptographic Message Syntax (CMS), also known as PKCS7. The full specification is defined in [RFC5652](https://datatracker.ietf.org/doc/html/rfc5652).

Honestly, I spent quite some time on this, but it's still over my head. Luckily, I found the `PKCS7_print_ctx` method provided by openssl library. It will print PKCS7 binary as a human readable string. This allows me to peek what's inside this blob. It includes the certificates up to the Root CA, signer information, algorithm, signing time, message digest, CDHash and so on.

## Provisioning Profile
Surprisingly, the provisioning profile does NOT present in `LC_CODE_SIGNATURE` in any form. Instead, it is copied into the app bundle as `embedded.mobileprovision` and hashed into `_CodeSignature/CodeResources`. In other words, code signing process doesn't treat provisioning profile differently than a regular resource file.

I also found it's not necessary to have `embedded.mobileprovision` in the app bundle when install a debug build to a device, which still bewilders me.

## More
This article merely covers the content and format of `LC_CODE_SIGNATURE`. There are a lot more beyond that. Especially, the actual code signing and validation are way more complicated.

Theses articles helped me a lot during the learning process, read them if you're interested.
* *OS Internals Volume III - Chapter 5: Code Signing
* [TN2206: macOS Code Signing In Depth](https://developer.apple.com/library/archive/technotes/tn2206/_index.html#//apple_ref/doc/uid/DTS40007919-CH1-TNTAG402)
* [A Deep Dive into iOS Code Signing](https://blog.umangis.me/a-deep-dive-into-ios-code-signing/)
* [Code Signing Requirement Language](https://developer.apple.com/library/archive/documentation/Security/Conceptual/CodeSigningGuide/RequirementLang/RequirementLang.html)
* [Inside Code Signing](https://www.objc.io/issues/17-security/inside-code-signing/)
* [A Warm Welcome to ASN.1 and DER](https://letsencrypt.org/docs/a-warm-welcome-to-asn1-and-der)
* [PKCS#7 - SignedData](http://www.pkiglobe.org/pkcs7.html)

