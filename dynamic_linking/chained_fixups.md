# Chained Fixups
Chained fixups is a new way to store information that will be used by `dyld`. Replacing `LC_DYLD_INFO(_ONLY)`, the chained fixups can save binary size and reduce launch time.

Traditionally, `dyld`, at launch time, needs to slide the fixed addresses with a random number, known as ASLR. This operation is called rebasing. Also, `dyld` needs to connect symbols from current binary with its linked dynamic libraries. This is called binding. Under the new format, both rebasing and binding have a new name, **fixup**, because they need to be "fixed up" before main function.

Chained fixups is [enabled by default if the binary is built for device and targeted at iOS 14+](https://github.com/qyang-nj/llios/blob/1f111edc87adbca68c336d3ab501e3ca4a1f2356/apple_open_source/ld64/src/ld/Options.cpp#L5233-L5238). We can also manually enable it by passing `-fixup_chains` to `ld`.

## `LC_DYLD_CHAINED_FIXUPS` and `LC_DYLD_EXPORTS_TRIE`
Once chained fixups is enabled, a Mach-O binary on longer uses `LC_DYLD_INFO`. Instead, it has two new load commands, `LC_DYLD_CHAINED_FIXUPS` and `LC_DYLD_EXPORTS_TRIE`. Interestingly, the lazy binding section `__DATA,__la_symbol_ptr` is gone too.

A quick recap of the old `LC_DYLD_INFO`. It contains four types of information:
* rebase -- addresses that need to be shifted by ASLR
* bind -- non-lazy bind symbols that are bound at launch time
* lazy bind -- lazy bind symbols that are bound at the first time of usage
* export -- exported symbols that are provided by this binary

In the new chained fixups, the export is moved to the new `LC_DYLD_EXPORTS_TRIE` load command but keeps the same trie format. The rebase and bind are chained together and are moved to `LC_DYLD_CHAINED_FIXUPS`. The lazy bind becomes bind (no more lazy binding)! As I have explained the export trie in detail [here](../exported_symbol/README.md), this article will focus on the `LC_DYLD_CHAINED_FIXUPS`.

## Layout
Since the format is new, I couldn't find any articles explaining the technical details. The best documentation I found is Apple source code, [mach-o/fixup-chain.h](https://github.com/qyang-nj/llios/blob/d204d56ff0533c1fae115b77e7554d2e6f4bc4aa/apple_open_source/dyld/include/mach-o/fixup-chains.h) and [otool/dylib_bind_info.c](https://github.com/qyang-nj/llios/blob/d204d56ff0533c1fae115b77e7554d2e6f4bc4aa/apple_open_source/cctools/otool/dyld_bind_info.c#L2906).

`LC_DYLD_CHAINED_FIXUPS` begins with `dyld_chained_fixups_header`, followed by `dyld_chained_starts_in_image`, which indicates the number of segment and where their `dyld_chained_starts_in_segment` are. The number of segment here equals to the number of `LC_SEGMENT_64`. Each `dyld_chained_starts_in_segment` contains an array of "page starts" of each page in the segment. A "page starts" is the first fixup in the page. The next fixup is at the location of current one + `next` field * 4. Each fixup is 64 bits and is either a rebase or a bind, indicated by the lowest bit. It's difficult to explain in plain text but relatively straightforward to read the code. The full parsing logic can be found in [macho_parser/chained_fixups.cpp](../macho_parser/sources/chained_fixups.cpp).

![Chained Fixups Layout](../articles/images/chained_fixups_layout.png)

## Advantages

### It saves binary size
Previously with `LC_DYLD_INFO(_ONLY)`, non-lazy bind addresses are always 64-bit 0x0 in the file, and the binding info is stored in a separate table in `__LINKEDIT`. (Read [this](./README.md) for more details.) This means those space with 0x0 are not efficiently used. With chained fixups, those addresses now store their own the binding information in 64 bit and the extra table is more compact. It's similar for rebase as well. In the Airbnb app I measured, the new format saves 1.4 MB.

```
# With traditional `LC_DYLD_INFO`, the `__got` section of `/bin/ls` are all zeros.
$ otool -s __DATA __got /bin/ls
/bin/ls:
Contents of (__DATA,__got) section
0000000100008008	00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0000000100008018	00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
0000000100008028	00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
```

### It reduces launch time
This article, [How iOS 15 makes your app launch faster](https://medium.com/geekculture/how-ios-15-makes-your-app-launch-faster-51cf0aa6c520), explains why app launch will become faster. (I was actually inspired by this article to research this topic.)

Besides what's mentioned in that article, I have another thought, but haven't been able to verify it. Since the rebase and bind data are chained together and organized by pages, it provides the ability to rebase address and bind symbols for any given pages independently. **It enables dyld to skip the mandatory fixups at launch time and only do it when the page is loaded (aka page fault).** Basically they all become lazy rebasing and binding. That's why there is no need for traditional lazy binding. If Apple doesn't do this today, it's a potential optimization for the future.

## Inspection

Unlike `LC_DYLD_INFO(_ONLY)`, `LC_DYLD_CHAINED_FIXUPS` can not be inspected by `dyldinfo` at this moment. You can use `otool -fixup_chains`, but it only shows imports, not rebase. My [macho parser](../macho_parser) is able to print out most of the information, including rebase and bind info of each segment, as well as the imports table.
```
./macho_parser -c LC_DYLD_CHAINED_FIXUPS sample.out
LC_DYLD_CHAINED_FIXUPS cmdsize: 16     dataoff: 0xc000 (49152)   datasize: 296
  CHAINED FIXUPS HEADER
    fixups_version : 0
    starts_offset  : 0x20 (32)
    imports_offset : 0x68 (104)
    symbols_offset : 0x88 (136)
    imports_count  : 8
    imports_format : 1 (DYLD_CHAINED_IMPORT)
    symbols_format : 0 (UNCOMPRESSED)

  IMPORTS
    [0] lib_ordinal: 4 (Foundation)        weak_import: 0   name_offset: 1 (_NSLog)
    [1] lib_ordinal: 254 (flat lookup)     weak_import: 1   name_offset: 8 (_c_extern_weak_function)
    [2] lib_ordinal: 1 (my_dylib.dylib)    weak_import: 0   name_offset: 32 (_my_dylib_func)
    [3] lib_ordinal: 2 (libSystem.B.dylib) weak_import: 0   name_offset: 47 (_printf)
    [4] lib_ordinal: 3 (CoreFoundation)    weak_import: 0   name_offset: 55 (___CFConstantStringClassReference)
    [5] lib_ordinal: 5 (libobjc.A.dylib)   weak_import: 0   name_offset: 89 (__objc_empty_cache)
    [6] lib_ordinal: 5 (libobjc.A.dylib)   weak_import: 0   name_offset: 108 (_OBJC_METACLASS_$_NSObject)
    [7] lib_ordinal: 5 (libobjc.A.dylib)   weak_import: 0   name_offset: 135 (_OBJC_CLASS_$_NSObject)

  SEGMENT __PAGEZERO (offset: 0)

  SEGMENT __TEXT (offset: 0)

  SEGMENT __DATA_CONST (offset: 24)
    size: 24
    page_size: 0x4000
    pointer_format: 6 (DYLD_CHAINED_PTR_64_OFFSET)
    segment_offset: 0x4000
    max_valid_pointer: 0
    page_count: 1
    page_start: 0
      PAGE 0 (offset: 0)
        0x00004000 BIND     ordinal: 0   addend: 0    reserved: 0   (_NSLog)
        0x00004008 BIND     ordinal: 1   addend: 0    reserved: 0   (_c_extern_weak_function)
        0x00004010 BIND     ordinal: 2   addend: 0    reserved: 0   (_my_dylib_func)
        0x00004018 BIND     ordinal: 3   addend: 0    reserved: 0   (_printf)
        0x00004020 BIND     ordinal: 4   addend: 0    reserved: 0   (___CFConstantStringClassReference)
        0x00004030 REBASE   target: 0x00003f83   high8: 0
        0x00004040 REBASE   target: 0x000080d8   high8: 0
        0x00004048 REBASE   target: 0x000080d8   high8: 0
    ...
```

