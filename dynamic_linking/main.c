
extern char *lib_str;
extern void lib_func(char *);

int main() {
    lib_func(lib_str);
    return 0;
}
