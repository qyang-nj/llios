#include<stdio.h>

__attribute__((constructor)) void c_constructor_function() {}

int main()
{
    printf("hello, world!\n");
    return 0;
}
