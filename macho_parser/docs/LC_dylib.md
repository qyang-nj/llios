# LC_*_DYLIB and LC_RPATH
There are several load commands that are used to support dynamic linking. Those are commands with suffix `DYLIB`, as well as command `LC_RPATH`.

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
An install name is actually a filepath embedded in a dynamic library. It tells the dynamic linker (`dyld`) where that library can be found at runtime. To specify the install name, we can pass `-install_name <path>` to the static linker (`ld`) at build time. For an existing library, we can use `install_name_tool` to change it.

**The install name is a property of the dylib itself**, so all the binaries linked against the same dylib contain the same search path. For example, `libswiftCore.dylib` has an install name "/usr/lib/swift/libswiftCore.dylib". When a binary links against `libswiftCore.dylib`, the linker will add `LC_LOAD_DYLIB` command, whose the content is "/usr/lib/swift/libswiftCore.dylib", to the binary. At runtime, the dynamic linker will look for `libswiftCore.dylib` at "/usr/lib/swift/".

The absolute path works for pre-installed system dylibs, but does not for a 3rd party one. They Mach-O way to work around this is to use some magic tokens.
* `@executable_path`: a placeholder for the executable path.
* `@loader_path`: a placeholder for the path of whatever causes the dylib to load. It can be an executable or a dylib.
* `@rpath`: a placeholder for the most flexible search path. (see `LC_RPATH` section below)

##### Learn more
[Linking and Install Names](https://www.mikeash.com/pyblog/friday-qa-2009-11-06-linking-and-install-names.html)

## LC_LOAD_DYLIB
Each `LC_LOAD_DYLIB` stores an install name of a dylib which is required by the binary. The binary can be an executable or another dylib. If the dylib cannot be found at runtime, the app will crash on launch.

## LC_LOAD_WEAK_DYLIB
Very similar to `LC_LOAD_DYLIB`, the only difference is that the dylib is weak (a.k.a optional). The app should handle the case when a weak dylib is missing.

## LC_REEXPORT_DYLIB
This is used for umbrella or facade library, which is not common in iOS app but is extensively used in the system libraries. For instance, `Foundation` re-exports `libobjc` and `CoreFoundation`. As an app binary, it only needs to link against `Foundation` and is able to use APIs from `libobjc` and `CoreFoundation` for free.

```
$ ./macho_parser Foundation.framework/Foundation --dylibs
LC_REEXPORT_DYLIB    cmdsize: 56     /usr/lib/libobjc.A.dylib
LC_REEXPORT_DYLIB    cmdsize: 96     /System/Library/Frameworks/CoreFoundation.framework/CoreFoundation
...
```

## LC_RPATH
``` c
struct rpath_command {
    uint32_t	 cmd;		/* LC_RPATH */
    uint32_t	 cmdsize;	/* includes string */
    union lc_str path;		/* path to add to run path */
};
```

The most important field of `LC_RPATH` is the file path, which is used to replace `@rpath` in the dylib install name. We can pass multiple `-rpath <path>` to the static linker, and each one results in a `LC_RPATH` load command in the final binary. For example, the `foo.dylib` has install name "@rpath/foo.dylib" and the binary has two rpaths,  "/path/to/a/" and "/path/to/b/". At launch time, `dyld` looks for `foo.dylib` at "/path/to/a/" first, then "/path/to/b/".

Using `@rpath` and `LC_RPATH` together enables the user to put the dylibs virtually anywhere.

