# Dynamic Interposing
Dynamic interposing is a mechanism that allows replacing exported (public) method implementations at runtime. [This article](https://www.emergetools.com/blog/posts/DyldInterposing) explains this topic very well. I'm going to try a sample and take a look at Mach-O level.

## Sample
The following sample interposes `open` system call.

``` c
// interpose.c
// clang -shared -o interpose.dylib interpose.c
#include <stdio.h>
#include <fcntl.h>

// This is copied from include/mach-o/dyld-interposing.h
#define DYLD_INTERPOSE(_replacement,_replacee) \
   __attribute__((used)) static struct{ const void* replacement; const void* replacee; } _interpose_##_replacee \
            __attribute__ ((section ("__DATA,__interpose"))) = { (const void*)(unsigned long)&_replacement, (const void*)(unsigned long)&_replacee };

static int my_open(const char* path, int flags, mode_t mode) {
    printf("Interposed open: %s\n", path);
    return open(path, flags, mode);
}

// Replace open with my_open
DYLD_INTERPOSE(my_open, open)
```

Then we use `DYLD_INSERT_LIBRARIES` environment variable to force load interpose.dylib. Because of some security features (I haven't done in-depth research on these), this works on the binaries installed through homebrew (e.g. [`exa`](https://github.com/ogham/exa)) but not on pre-installed system binaries (e.g `/bin/ls`). As we can see below, the implementation of `open` method is replaced.

```
$ DYLD_INSERT_LIBRARIES=/Users/qyang/Projects/llios/macho_parser/interpose.dylib exa
Interposed open: /opt/homebrew/Cellar/exa/0.10.1/bin
Interposed open: /opt/homebrew/Cellar/exa/0.10.1/bin/Info.plist
Interposed open: /dev/autofs_nowait
Interposed open: /Users/user/.CFUserTextEncoding
Interposed open: /dev/autofs_nowait
Interposed open: /Users/qyang/.CFUserTextEncoding
...
```

There is a hidden environment variable `DYLD_PRINT_INTERPOSING`, which is not listed in `man dyld`. We can set it to get more details about the interposing at runtime.
```
DYLD_PRINT_INTERPOSING=1 DYLD_INSERT_LIBRARIES=/Users/qyang/Projects/llios/macho_parser/interpose.dylib exa
dyld[49176]: interpose.dylib has interposed '_open' to replacing binds to 0x197A19FB8 with 0x1003FFF30
dyld[49176]:   interpose replaced 0x197A19FB8 with 0x197A19FB8 in /Users/qyang/Projects/llios/macho_parser/interpose.dylib
dyld[49176]:   interpose replaced 0x197A19FB8 with 0x1003FFF30 in /opt/homebrew/Cellar/exa/0.10.1/bin/exa
...
```

## Mach-O
Using [macho parser](../macho_parser/README.md), we found that `interpose.dylib` has a section `__DATA,__interpose`.
```
$ macho_parser interpose.dylib --segments
...
LC_SEGMENT_64        cmdsize: 152    segname: __DATA         file: 0x00008000-0x0000c000 16.00KB    vm: 0x000008000-0x00000c000 16.00KB   prot: 3/3
   5: 0x000008000-0x000008010 16B         (__DATA,__interpose)              type: S_REGULAR  offset: 32768
...
```
This section contains an array of tuple, which is a pair of function addresses (replacement, replacee).
```
$ xxd -s 32768 -l 16 /Users/qyang/Projects/llios/macho_parser/interpose.dylib
00008000: 303f 0000 0000 1000 0000 0000 0000 0080
          ─────────┬───────── ─────────┬─────────
                   │                   └─ address of replacee (open from libSystem)
                                          This address needs to be bound if the replacee is from another dylib.
                   └─ address of replacement (my_open from interpose.dylib)
                      This address needs to be rebased.
```

Because of the addresses in `__DATA,__interpose` need to be bound or rebased, **interposing will happen after fix-up**.

## dyld
[This is the snippet](https://github.com/qyang-nj/llios/blob/badce36ff3ccc8dc10e937e7d55aef8e5450673d/apple_open_source/dyld/src/ImageLoaderMachO.cpp#L1228-L1276) from dyld source code that directly processes `__DATA,__interpose` section. Most of the code is pretty self explanatory, except one line being a little confusing.

``` c
tuple.neverImage = this;
```

After looking at [the definition of `struct InterposeTuple`](https://github.com/qyang-nj/llios/blob/badce36ff3ccc8dc10e937e7d55aef8e5450673d/apple_open_source/dyld/src/ImageLoader.h#L703-L708), we can have a pretty good guess.

``` c
struct InterposeTuple {
    uintptr_t                                       replacement;
    dyld3::AuthenticatedValue<const ImageLoader*>   neverImage;		// don't apply replacement to this image
    dyld3::AuthenticatedValue<const ImageLoader*>   onlyImage;		// only apply replacement to this image
    uintptr_t                                       replacee;
};
```

It means the method replacement won't be applied to the binary that defines the interpose. In the sample above, the `open` function inside `interpose.dylb` itself will invoke the `libSystem`'s `open`, while the `open` function in other images (dylib or executable) will be replaced by `my_open` from `interpose.dylb`. Unlike Objective-C's swizzling or interposing on Linux, we can have a clean code to call the original implementation.

Other than hard-coded section, `dyld` also provides the API [`dyld_dynamic_interpose`](https://github.com/qyang-nj/llios/blob/badce36ff3ccc8dc10e937e7d55aef8e5450673d/apple_open_source/dyld/dyld3/APIs.h#L180) to allow interposing at runtime.
