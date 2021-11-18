# LC_*_DYLIB and LC_RPATH
These load commands are related to dynamic linking at runtime.

## LC_ID_DYLIB / LC_LOAD_DYLIB / LC_LOAD_WEAK_DYLIB / LC_REEXPORT_DYLIB
``` c
struct dylib_command {
    uint32_t     cmd;       /* LC_ID_DYLIB, LC_LOAD_{,WEAK_}DYLIB, LC_REEXPORT_DYLIB */
    uint32_t     cmdsize;   /* includes pathname string */
    struct dylib dylib;     /* the library identification */
};

struct dylib {
    union lc_str  name;             /* library's path name */
    uint32_t timestamp;             /* library's build time stamp */
    uint32_t current_version;       /* library's current version number */
    uint32_t compatibility_version; /* library's compatibility vers number*/
};
```

All these `LC_*_DYLIB` commands use the same `dylib_command` struct. `LC_ID_DYLIB` exists in the dynamic library (`MH_DYLIB`), and `LC_LOAD_DYLIB` and `LC_LOAD_WEAK_DYLIB` are in the binary (could be any macho-o type) that links dynamic libraries.

### Install name
The most important field of `struct dylib` is `name`, library's path name, aka, install name. An install name is just a filepath embedded within a dynamic library which tells the linker where that library can be found at runtime. To specify the install name, we pass `-install_name <path>` to the static linker at build time. For an existing library, we can use `install_name_tool` to change it.

It's worth noting that install name is a property of the dylib itself, so all the binaries linked against the same dylib contain the same install name. One way to work around this is to use `@rpath`, which let the dylib user to customize the search path (see `LC_RPATH` section below).

##### Learn more
[Linking and Install Names](https://www.mikeash.com/pyblog/friday-qa-2009-11-06-linking-and-install-names.html)

## LC_RPATH
``` c
struct rpath_command {
    uint32_t	 cmd;		/* LC_RPATH */
    uint32_t	 cmdsize;	/* includes string */
    union lc_str path;		/* path to add to run path */
};
```

The only meaningful thing that `LC_RPATH` has is a filepath. Dynamic linker will use that to replace `@rpath` in the dylib install name to search the dylib file. We can pass multiple `-rpath <path>` to the static linker, and each one results in a `LC_RPATH` is the final binary.

