# LC_ENCRYPTION_INFO_64
After we upload an app to App Store, Apple will encrypt the app using its own FairPlay DRM. This is reflected in `LC_ENCRYPTION_INFO_64` load command.

``` c
struct encryption_info_command_64 {
   uint32_t cmd;        /* LC_ENCRYPTION_INFO_64 */
   uint32_t cmdsize;    /* sizeof(struct encryption_info_command_64) */
   uint32_t cryptoff;   /* file offset of encrypted range */
   uint32_t cryptsize;  /* file size of encrypted range */
   uint32_t cryptid;    /* which enryption system, 0 means not-encrypted yet */
   uint32_t pad;        /* padding to make this struct's size a multiple of 8 bytes */
};
```

The field `cryptoff` and `cryptsize` define the range that is or will be encrypted. From the one app I looked, this range entirely falls into `__TEXT` segment. It starts with the first section (__TEXT,__text) and ends with the second last section, excluding (__TEXT,__oslogstring). I think we can safely say only code in `__TEXT` segment are encrypted.

I have compared the app binary before uploading to App Store and the binary downloaded from App Store [using Apple Configurator 2](https://medium.com/xcnotes/how-to-download-ipa-from-app-store-43e04b3d0332). The field `cryptid` is changed from 0 to 1, while other fields remain unchanged. According to [the source code of `dyld`](https://github.com/qyang-nj/llios/blob/6e7b14b5a4c97fb42f6485c8e71e24079ec91f63/apple_open_source/dyld/dyld3/MachOFile.cpp#L1653-L1666), this bit is used to indicate whether the binary is FairPlay encrypted.

Also from [the dyld source code](https://github.com/qyang-nj/llios/blob/6e7b14b5a4c97fb42f6485c8e71e24079ec91f63/apple_open_source/dyld/src/ImageLoaderMachOCompressed.cpp#L2144), `mremap_encrypted` is the method that is used for decryption. There is a tool called [UnFairPlay](https://github.com/subdiox/UnFairPlay) which is using that method to decrypt an encrypted binary.

As FairPlay is a closed-source algorithm, there isn't much articles online talking about how it actually works.
