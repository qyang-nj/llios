# LC_DYLD_INFO(_ONLY)
As their names suggest, the load command `LC_DYLD_INFO` and `LC_DYLD_INFO_ONLY` store the information that is used by `dyld`, the dynamic linker. Those information consists of rebase, binding and export symbols. All of them can be accessed through `struct dyld_info_command` and be inspected by `xcrun dyldinfo (-rebase|-bind|-weak_bind|-lazy_bind|-export)`.

``` c
struct dyld_info_command {
    uint32_t cmd;            /* LC_DYLD_INFO or LC_DYLD_INFO_ONLY */
    uint32_t cmdsize;        /* sizeof(struct dyld_info_command) */

    uint32_t rebase_off;     /* file offset to rebase info  */
    uint32_t rebase_size;    /* size of rebase info   */

    uint32_t bind_off;       /* file offset to binding info   */
    uint32_t bind_size;      /* size of binding info  */

    uint32_t weak_bind_off;  /* file offset to weak binding info   */
    uint32_t weak_bind_size; /* size of weak binding info  */

    uint32_t lazy_bind_off;  /* file offset to lazy binding info */
    uint32_t lazy_bind_size; /* size of lazy binding infs */

    uint32_t export_off;     /* file offset to lazy binding info */
    uint32_t export_size;    /* size of lazy binding infs */
};
```
*(The above snippet is directly copied from `<mach-o/loder.h>`. Apparently, the comments on `export_off/size` are incorrect.)*

The difference between these two commands is that `LC_DYLD_INFO_ONLY` has `LC_REQ_DYLD` bit set, which causes `dyld` refuses the load the image if the load command cannot be understood.
``` c
#define	LC_DYLD_INFO        0x22
#define	LC_DYLD_INFO_ONLY   (0x22|LC_REQ_DYLD)
```

## Bind
Binding is the process to resolve undefined symbols in a binary. This is required when a binary depends on another binary, usually an executable depending on a dynamic library. Biding information stored in `LC_DYLD_INFO(_ONLY)` is used to facilitate this process.

As we can tell from `struct dyld_info_command`, there are three kinds of binding, non-lazy binding, lazy binding and weak binding. They are encoded in the similar way, known as opcodes. More on this is at [Binding Information](../../dynamic_linking/docs/BindingInfo.md).

## Rebase
Rebasing is needed to support ASLR (Address Space Layout Randomization), in which the addresses in a binary are shifted by a random number. Rebase information is used to facilitate that process, telling `dyld` what addresses are needed to be rebased at launch time.

Using macho-o parser, we can tell that all rebases are in the `__DATA` or `__DATA_CONST` segment. It makes sense because `__TEXT` segment is read only. But why code in `__TEXT` doesn't need to be rebased? It's because all addresses in `__TEXT` are relative to the instruction pointer. This is called [RIP-relative addressing](). Only absolute addresses need to be shifted.

The way that rebase information is encoded is very similar to binding, which is also using opcodes. Check out [Binding Information](../../dynamic_linking/docs/BindingInfo.md) for details.

## Export
If we say binding info stores symbols that a binary depends upon, then export info saves the symbols that a binary provides. Instead of opcodes used by binding and rebasing, export info uses a fancy data structure, trie. The deep dive of export info is at [Exported Symbols](../../exported_symbol/README.md).
