extern int test_gameboy_init(void);
extern void test_gameboy_main(void);
extern void test_gameboy_fini(void);
__attribute__((visibility("default"))) int app_module_init(void){return test_gameboy_init();}
__attribute__((visibility("default"))) void app_main(void){test_gameboy_main();}
__attribute__((visibility("default"))) void app_module_fini(void){test_gameboy_fini();}
