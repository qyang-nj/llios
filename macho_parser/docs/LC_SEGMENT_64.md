# LC_SEGMENT_64
A `LC_SEGMENT_64` defines a segment, which basically is a chunk of continuous space that will be `mmap`'d to the memory. Most segments are well-known, like `__TEXT` and `__DATA`. Besides their names, the key different among segments is the memory protection mode (`initprot` and `maxprot`).
``` c
struct segment_command_64 {     /* for 64-bit architectures */
    uint32_t    cmd;            /* LC_SEGMENT_64 */
    uint32_t    cmdsize;        /* includes sizeof section_64 structs */
    char        segname[16];    /* segment name */
    uint64_t    vmaddr;         /* memory address of this segment */
    uint64_t    vmsize;         /* memory size of this segment */
    uint64_t    fileoff;        /* file offset of this segment */
    uint64_t    filesize;       /* amount to map from the file */
    vm_prot_t   maxprot;        /* maximum VM protection */
    vm_prot_t   initprot;       /* initial VM protection */
    uint32_t    nsects;         /* number of sections in segment */
    uint32_t    flags;          /* flags */
};
```

## __PAGEZERO
`__PAGEZERO` segment is **not** readable (`VM_PROT_READ`), writable (`VM_PROT_WRITE`) or executable (`VM_PROT_EXECUTE`). It has zero size on disk but X size in virtual memory. Its main purpose is to trap NULL dereference, causing segment fault. By default, `__PAGEZERO` segment is [4KB on 32-bit systems and 4GB on 64-bit systems](https://github.com/qyang-nj/llios/blob/a61ad95bca9df1c0085d78b0b3165efc8b83f791/apple_open_source/ld64/src/ld/Options.cpp#L6113-L6131). The size can be changed by `-pagezero_size` linker flag.

Please note this segment can catch common programmer errors but not intentional violations. For example, although the following code is dereferencing a NULL pointer `a`, it won't crash, as `a[i]` is out of the `__PAGEZERO` segment.

``` c
#include <stdio.h>
int main() {
    char *a = NULL;
    char *b = "hello";
    int64_t i = (int64_t)b;

    printf("%p %c\n", b, a[i]); // deference a NULL pointer but its value is 'h'.
}
```

## __TEXT
`__TEXT` segment is readable (`VM_PROT_READ`) and executable (`VM_PROT_EXECUTE`), but not writable (`VM_PROT_WRITE`). It starts at the beginning of the file and includes Mach-O header and load commands, as those are also read-only.

### __text
This section is where the executable code is. It usually starts at the beginning of a whole page. If the page size is 0x4000, which is usually enough for the Mach-O header, load commands and paddings, the `__text` section starts at the second page, 0x4000 from the file.

### __cstring
This section stores constant string literals. The following code will end up having "hello world" in this section.
``` c
printf("hello world");
```

## __DATA
`__DATA` segment is readable (`VM_PROT_READ`) and writable (`VM_PROT_WRITE`) but not executable (`VM_PROT_EXECUTE`), which makes it useful for mutable data. Fox example, lay binding (`__la_symbol_ptr`).

### __la_symbol_ptr
This section is used to store lazy symbol pointers. It's used for lazy binding during [dynamic linking](../../dynamic_linking).

### __interpose
This section contains an array of tuple. Each tuple is a pair of function addresses. See [dynamic interposing](../../dynamic_linking/dynamic_interposing.md).

## __DATA_CONST
`__DATA_CONST` segment stores constant data, some of which needs to be initialized. At the time of `mmap`, `__DATA_CONST`, same as `__DATA`, is readable and writable. Once initialized, `dyld` will change this segment to just readable via `mprotect`. Then it becomes real constant. One of use cases for this is the non-lazy biding (`__got`).

### __got
This section stores global Offset table. See [dynamic linking](../../dynamic_linking).

### __mod_init_func
This is the section that contains of a list of function pointers, which will [be executed by `dyld`](https://github.com/opensource-apple/dyld/blob/3f928f32597888c5eac6003b9199d972d49857b5/src/ImageLoaderMachO.cpp#L1815~L1847) **before `main`**. Those functions are called constructor and they will affect the app launch time.

``` c
// Function with "constructor" attribute will be added to __DATA,__mod_init_func section
__attribute__((constructor)) void c_constructor_function() {}
```

Once we have the function pointer address, we can use `atos` to query the function name.
```
> xcrun atos -o sample.out 0x100003f20
c_constructor_function (in sample) + 0
```

Similar to constructor methods, ObjC's `+load` methods will also be executed before `main`, but uses a different mechanism. See below `__objc_nlclslist` section.

### __objc_classlist / __objc_nlclslist
The mach-o binaries that are compiled from Objective-C code have two sections, `(__DATA_CONST,__objc_classlist)` and `(__DATA_CONST,__objc_nlclslist)`. `__objc_classlist` includes the addresses of all ObjC classes, while `__objc_nlclslist` contains only *non-lazy* classes. [Non-lazy classes are classes that have `+load` method](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-runtime-new.mm#L2806~L2812) and will be loaded at launch time.

How `+load` is executed during startup? First, dyld calls [_objc_init](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-os.mm#L803~L831), where [a notification is registered when an image is loaded](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-os.mm#L830). Then, in the notification callback, [load_images](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-runtime-new.mm#L2157~L2193), it [calls the load methods](https://github.com/opensource-apple/objc4/blob/cd5e62a5597ea7a31dccef089317abb3a661c154/runtime/objc-loadmethod.mm#L306~L365) in that image.

The difference between `+load` and `__mod_init_func` is that the former guarantees [certain order of execution](https://developer.apple.com/documentation/objectivec/nsobject/1418815-load?language=objc), while the latter doesn't.

**Learn more**
* [Objective-C: What is a lazy class?](https://stackoverflow.com/a/15318325/3056242)
* [What did Runtime do during the startup of Runtime objc4-779.1 App?](https://programmer.group/what-did-runtime-do-during-the-startup-of-runtime-objc4-779.1-app.html)

## __LINKEDIT
`__LINKEDIT` segment contains data that's used by the linker. Unlike other segments, this one doesn't have sections. Its contents are described by other load commands, e.g. `LC_SYMTAB`, `LC_DYSYMTAB`.

## __LLVM_COV
`__LINKEDIT` is used for code coverage.

### __llvm_covfun

### __llvm_covmap
See [CoverageMapping.h](https://github.com/llvm/llvm-project/blob/main/llvm/include/llvm/ProfileData/Coverage/CoverageMapping.h)
