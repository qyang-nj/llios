# LC_*_DYLIB and LC_RPATH
There are a few load commands that are used to support dynamic linking. Those are commands with suffix `DYLIB`, as well as command `LC_RPATH`.

All `LC_*_DYLIB` commands uses the same `struct dylib_command` and `struct dylib`.
``` c
struct dylib_command {
    uint32_t     cmd;               /* LC_ID_DYLIB, LC_LOAD_{,WEAK_}DYLIB, LC_REEXPORT_DYLIB */
    uint32_t     cmdsize;           /* includes pathname string */
    struct dylib dylib;             /* the library identification */
};

struct dylib {
    union lc_str  name;             /* library's path name */
    uint32_t timestamp;             /* library's build time stamp */
    uint32_t current_version;       /* library's current version number */
    uint32_t compatibility_version; /* library's compatibility vers number*/
};
```

## LC_ID_DYLIB
`LC_ID_DYLIB` only exists in dynamic libraries (.dylib), whose mach-o type is `MH_DYLIB`. This command stores the install name of the dylib.

### Install name
An install name is actually a filepath embedded within a dynamic library. It tells the dynamic linker (`dyld`) where that library can be found at runtime. To specify the install name, we can pass `-install_name <path>` to the static linker (`ld`) at build time. For an existing library, we can use `install_name_tool` to change it.

The install name is a property of the dylib itself, so all the binaries linked against the same dylib contain the same search path. For example, `libswiftCore.dylib` has an install name "/usr/lib/swift/libswiftCore.dylib". When a binary links against `libswiftCore.dylib`, the linker will add `LC_LOAD_DYLIB` command, whose the content is "/usr/lib/swift/libswiftCore.dylib", to the binary. At runtime, the dynamic linker will look for `libswiftCore.dylib` at "/usr/lib/swift/".

The absolute path works for pre-installed system dylibs, but it won't work with a 3rd party dylib. One way to work around this is to use `@rpath`, which let the dylib user to customize the search path (see `LC_RPATH` section below).

##### Learn more
[Linking and Install Names](https://www.mikeash.com/pyblog/friday-qa-2009-11-06-linking-and-install-names.html)

## LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB
Each `LC_LOAD_DYLIB` and `LC_LOAD_WEAK_DYLIB` stores an install name of a dylib which is used by the binary. The binary can be an executable or another dylib.

`LC_LOAD_DYLIB` indicates the dylib is required. If the dylib cannot be found at runtime, the app will crash on launch. Conversely, `LC_LOAD_WEAK_DYLIB` indicates the dylib is optional. The app should handle the case when a weak dylib is missing.

## LC_REEXPORT_DYLIB

## LC_RPATH
``` c
struct rpath_command {
    uint32_t	 cmd;		/* LC_RPATH */
    uint32_t	 cmdsize;	/* includes string */
    union lc_str path;		/* path to add to run path */
};
```

The only meaningful thing that `LC_RPATH` has is a filepath. Dynamic linker will use that to replace `@rpath` in the dylib install name to search the dylib file. We can pass multiple `-rpath <path>` to the static linker, and each one results in a `LC_RPATH` is the final binary.

