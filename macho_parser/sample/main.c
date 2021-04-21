#include<stdio.h>

// from my_dylib
extern void my_dylib_func();

// Function with "constructor" attribute will be added to __DATA,__mod_init_func section
__attribute__((constructor)) void c_constructor_function() {}

// Used symbols will be marked as N_NO_DEAD_STRIP in n_desc in the objective file.
__attribute__((used)) void c_used_function() {}

// Weak symbols will be marked as N_WEAK_REF and DYNAMIC_LOOKUP_ORDINAL in n_desc.
__attribute__((weak)) extern void c_extern_weak_function();

//
__attribute__((weak_import)) void c_weak_import_function() {}

int main() {
    // String literal will be added to __TEXT,__cstring section
    printf("hello, world!\n");

    my_dylib_func();
    c_weak_import_function();

    if (c_extern_weak_function != NULL) {
        c_extern_weak_function();
    }

    return 0;
}
