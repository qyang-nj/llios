# Code Coverage On Test Crash
(The steps of gathering code coverage is described [here](../articles/CodeCoverage.md#build-and-run).)

The process of code coverage gathering involves generating a `.profraw` file, which contains instrument data, upon completing the test. This `.profraw` file is then processed to create the coverage report. However, if a test prematurely crashes, this process falls apart. Despite the test runner's ability to continue with the remaining test cases, the coverage data compiled up to the point of the crash is lost.

Upon investigating the source code ([InstrProfilingFile.c](https://github.com/llvm/llvm-project/blob/f7ab79f33ef5609a2d8519cbfc676842d617eeb3/compiler-rt/lib/profile/InstrProfilingFile.c)), I discovered that the function [`__llvm_profile_write_file`](https://github.com/llvm/llvm-project/blob/f7ab79f33ef5609a2d8519cbfc676842d617eeb3/compiler-rt/lib/profile/InstrProfilingFile.c#L1033) is responsible for writing to the profraw file. This function, [registered](https://github.com/llvm/llvm-project/blob/f7ab79f33ef5609a2d8519cbfc676842d617eeb3/compiler-rt/lib/profile/InstrProfilingFile.c#L1166) in the [`atexit`](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/atexit.3.html) handler, is triggered either when `exit()` is called or when `main()` returns, but not when `abort()` is called. Hence, this function isn't executed when the program encounters a crash, explaining a 0 byte `.profraw` file we saw following a crash.

This seemed unfortunate until I noticed something while closely examining the function body and saw this,
```c
if (lprofProfileDumped() || __llvm_profile_is_continuous_mode_enabled()) {
    PROF_NOTE("Profile data not written to file: %s.\n", "already written");
    return 0;
}
```

The "continuous mode" sounds interesting, so I delved in deeper and found that the continuous mode can be [enabled](https://github.com/llvm/llvm-project/blob/f7ab79f33ef5609a2d8519cbfc676842d617eeb3/compiler-rt/lib/profile/InstrProfilingFile.c#L771) by adding "**%c**" into the profraw file name. Honestly, it's a quite weird way to enable a feature. As the name implies, the continuous mode allows for continuously writing to the profraw file instead of waiting until the end. This is exactly what we need.

I later discovered that this feature is actually [documented](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#running-the-instrumented-program).
> “%c” expands out to nothing, but enables a mode in which profile counter updates are continuously synced to a file. This means that if the instrumented program crashes, or is killed by a signal, perfect coverage information can still be recovered. Continuous mode does not support value profiling for PGO, and is only supported on Darwin at the moment. Support for Linux may be mostly complete but requires testing, and support for Windows may require more extensive changes: please get involved if you are interested in porting this feature.

### TL;DR
> [!IMPORTANT]
> Always use "**%c**" in the file name specified in `LLVM_PROFILE_FILE`, so that we don't loose the code coverage when test crashes.

### References
[Demystifying the profraw format](https://leodido.dev/demystifying-profraw/)
