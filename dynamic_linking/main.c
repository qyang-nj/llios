
extern char *llios_lib_str;
extern void llios_lib_func(char *);

int main() {
    llios_lib_func(llios_lib_str);
    return 0;
}
