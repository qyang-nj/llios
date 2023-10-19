# Code Coverage On Test Crash
(The steps of gathering code coverage is described [here](../articles/CodeCoverage.md#build-and-run).)

The process of code coverage gathering involves generating a `.profraw` file, which contains instrument data, upon completing the test. This `.profraw` file is then processed to create the coverage report. However, if a test prematurely crashes, this process falls apart. Despite the test runner's ability to continue with the remaining test cases, the coverage data compiled up to the point of the crash is lost.

Upon investigating the source code ([InstrProfilingFile.c](https://github.com/llvm/llvm-project/blob/f7ab79f33ef5609a2d8519cbfc676842d617eeb3/compiler-rt/lib/profile/InstrProfilingFile.c)), I discovered that the function [`__llvm_profile_write_file`](https://github.com/llvm/llvm-project/blob/f7ab79f33ef5609a2d8519cbfc676842d617eeb3/compiler-rt/lib/profile/InstrProfilingFile.c#L1033) is responsible for writing to the profraw file. This function, registered in the [`atexit`](https://developer.apple.com/library/archive/documentation/System/Conceptual/ManPages_iPhoneOS/man3/atexit.3.html) handler, is triggered either when `exit()` is called or when `main()` returns, but not when `abort()` is called. Hence, this function isn't executed when the program encounters a crash, explaining a 0 byte .profraw file we saw following a crash.

This situation seemed quite unfortunate until I noticed something while closely examining the function body and saw this.
```c
if (lprofProfileDumped() || __llvm_profile_is_continuous_mode_enabled()) {
    PROF_NOTE("Profile data not written to file: %s.\n", "already written");
    return 0;
}
```

The term "continuous mode" is interesting, so I delved in deeper and found that the continuous mode can be activated by adding "**%c**" into the profraw file name. Honestly, it's a quite weird way to enable a feature. As the name implies, the continuous mode allows for continuously writing to the profraw file instead of waiting until the end. This exactly what we need.

I later discovered that this feature is actually [documented](https://clang.llvm.org/docs/SourceBasedCodeCoverage.html#running-the-instrumented-program).

### TL;DR
> [!IMPORTANT]
> Always use "%c" when specifying `LLVM_PROFILE_FILE`, so that we don't loose the code coverage when test crashes.

### References
[Demystifying the profraw format](https://leodido.dev/demystifying-profraw/)
