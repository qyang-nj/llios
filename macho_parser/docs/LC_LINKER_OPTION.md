# LC_LINKER_OPTION

`LC_LINKER_OPTION` only exists in the object files (`MH_OBJECT`) and is used for auto-linking. This load command literally contains linker flags that will be used by the static linker.

``` c
/*
 * The linker_option_command contains linker options embedded in object files.
 */
struct linker_option_command {
    uint32_t  cmd;      /* LC_LINKER_OPTION only used in MH_OBJECT filetypes */
    uint32_t  cmdsize;
    uint32_t  count;    /* number of strings */
    /* concatenation of zero terminated UTF8 strings. Zero filled at end to align */
};
```

##### Learn more
[Auto Linking on iOS & macOS](https://milen.me/writings/auto-linking-on-ios-and-macos/)
