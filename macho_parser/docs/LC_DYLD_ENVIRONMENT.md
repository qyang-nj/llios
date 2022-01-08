# LC_DYLD_ENVIRONMENT

``` c
#define LC_DYLD_ENVIRONMENT 0x27 /* string for dyld to treat like environment variable */

struct dylinker_command {
	uint32_t     cmd;       /* LC_ID_DYLINKER, LC_LOAD_DYLINKER or LC_DYLD_ENVIRONMENT */
	uint32_t     cmdsize;   /* includes pathname string */
	union lc_str name;      /* dynamic linker's path name */
};
```

It's pretty well known that we can provide library/framework search paths at run time through environment variables like `DYLD_FRAMEWORK_PATH` and `DYLD_LIBRARY_PATH`, but it's less known that we can embed those environment variables into the binary. To do so, we add `-dyld_env ENV=VALUE` to the linker at build time. Please note that [only the environment variables begging with `DYLD_` and ending with `_PATH` will have effects](https://github.com/qyang-nj/llios/blob/c53e5b0e92f7783c02bea0864afd4cab17cbbb8f/apple_open_source/dyld/src/dyld2.cpp#L2340).

I did see a few binaries inside Xcode containing this load command, but it's unclear to me what the real problem these commands are addressing.
```
$./macho_parser /Applications/Xcode-13.1.0.app/Contents/MacOS/Xcode
...
LC_DYLD_ENVIRONMENT  cmdsize: 120    DYLD_VERSIONED_FRAMEWORK_PATH=@executable_path/../SystemFrameworks:@executable_path/../InternalFrameworks
LC_DYLD_ENVIRONMENT  cmdsize: 120    DYLD_VERSIONED_LIBRARY_PATH=@executable_path/../SystemLibraries:@executable_path/../InternalLibraries
...
```



