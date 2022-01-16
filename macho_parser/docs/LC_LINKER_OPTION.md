# LC_LINKER_OPTION

`LC_LINKER_OPTION` is used for auto-linking and only exists in the object files (`MH_OBJECT`). This load command contains linker flags that will be used by the static linker.

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

Swift uses auto linking extensively. Here are some examples.
``` bash
$ touch foo.swift # create an empty swift file
$ xcrun swiftc -c foo.swift -o foo.o
$ macho_parser -c LC_LINKER_OPTION foo.o
LC_LINKER_OPTION     cmdsize: 40     count: 1   -lswiftSwiftOnoneSupport
LC_LINKER_OPTION     cmdsize: 24     count: 1   -lswiftCore
LC_LINKER_OPTION     cmdsize: 32     count: 1   -lswift_Concurrency
LC_LINKER_OPTION     cmdsize: 24     count: 1   -lobjc
```
```bash
$ echo "import Security" > foo.swift
$ xcrun swiftc -c foo.swift -o foo.o
$ macho_parser -c LC_LINKER_OPTION foo.o
LC_LINKER_OPTION     cmdsize: 32     count: 2   -framework Security
LC_LINKER_OPTION     cmdsize: 32     count: 1   -lswiftDarwin
LC_LINKER_OPTION     cmdsize: 32     count: 1   -lswift_Concurrency
LC_LINKER_OPTION     cmdsize: 24     count: 1   -lswiftCore
LC_LINKER_OPTION     cmdsize: 40     count: 1   -lswiftCoreFoundation
LC_LINKER_OPTION     cmdsize: 40     count: 2   -framework CoreFoundation
LC_LINKER_OPTION     cmdsize: 32     count: 1   -lswiftDispatch
LC_LINKER_OPTION     cmdsize: 32     count: 1   -lswiftObjectiveC
LC_LINKER_OPTION     cmdsize: 32     count: 2   -framework Combine
LC_LINKER_OPTION     cmdsize: 24     count: 1   -lswiftXPC
LC_LINKER_OPTION     cmdsize: 40     count: 1   -lswiftSwiftOnoneSupport
LC_LINKER_OPTION     cmdsize: 24     count: 1   -lobjc
```

In the above example, even though we imported Security framework in the code, we didn't specify that framework to the compiler. During **Sema** phase, the swift compiler parses the import statements and finds the module in the provided search paths. (The SDK path is inferred in the above example, so we didn't add any search path.) Then the compiler puts the relevant linker flags in the `LC_LINKER_OPTION` load commands. When the static linker sees them, they're treated as the same flags as passed through command line. Therefore, we don't have to specify "Security" in the entire build process.


##### Learn more
[Auto Linking on iOS & macOS](https://milen.me/writings/auto-linking-on-ios-and-macos/)
