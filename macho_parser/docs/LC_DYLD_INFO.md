# LC_DYLD_INFO(_ONLY)
As their names suggest, the load command `LC_DYLD_INFO` and `LC_DYLD_INFO_ONLY` store the information that is used by `dyld`, including rebase, binding and export symbols. All these information is stored in `struct dyld_info_command` and can be inspected by `xcrun dyldinfo (-rebase|-bind|-weak_bind|-lazy_bind|-export)`.

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

## Rebase

## Bind

## Export Info
A deep dive of export info is at "[exported_symbol](../exported_symbol)".
