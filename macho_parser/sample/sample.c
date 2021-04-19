#include<stdio.h>

// Function with "constructor" attribute will be added to __DATA,__mod_init_func section
__attribute__((constructor)) void c_constructor_function() {}

// Symbols with "used" attribute will be marked as no dead strip in the objective file.
__attribute__((used)) static void c_used_function() {}

int main()
{
    // String literal will be added to __TEXT,__cstring section
    printf("hello, world!\n");
    return 0;
}
