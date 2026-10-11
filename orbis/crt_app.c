/* Managed-app startup — C port of ps5-native-app-boilerplate app_crt.cpp,
 * with Simpsons-PS5-Builder refinements: IEEE FP state before libc, a
 * durable startup.log marker before any constructor runs, per-initializer
 * address logging so a faulting ctor names itself, and a null-params trap.
 * The Orbis rtld invokes the module's DT_INIT (=_init) / DT_FINI (=_fini).
 */
#include <stddef.h>
#include <stdint.h>

typedef void (*func_t)(void);

extern void _init_env(void *process_parameters);
extern int main(int argc, char **argv);
extern int atexit(func_t callback);
extern int sceSystemServiceLoadExec(const char *path, char *const argv[]);
extern void ps5_fp_ieee(void);
extern int sceKernelDebugOutText(int channel, const char *text);
void id4_startup_log(const char *message);
void id4_log_number(const char *prefix, uint64_t value);

extern func_t __preinit_array_start[] __attribute__((weak));
extern func_t __preinit_array_end[]   __attribute__((weak));
extern func_t __init_array_start[]    __attribute__((weak));
extern func_t __init_array_end[]      __attribute__((weak));
extern func_t __fini_array_start[]    __attribute__((weak));
extern func_t __fini_array_end[]      __attribute__((weak));

__attribute__((weak)) void catchReturnFromMain(int status) { (void)status; }

static void run_forward(func_t *first, func_t *last)
{
    if (!first || !last)
        return;
    while (first != last) {
        /* If a constructor faults, the last klog line names its address. */
        id4_log_number("CRT: calling initializer at ", (uintptr_t)*first);
        (*first++)();
    }
}

void id4_metrics_start(void);

void _init(void)
{
    id4_startup_log("CRT: preinitializers begin");
    run_forward(__preinit_array_start, __preinit_array_end);
    id4_startup_log("CRT: global constructors begin");
    run_forward(__init_array_start, __init_array_end);
    id4_startup_log("CRT: global constructors completed");
    /* Metrics reporter runs before main like theirs — a hang in main still
     * leaves heartbeats naming when it froze. */
    id4_metrics_start();
}

void _fini(void)
{
    id4_startup_log("CRT: finalizers begin");
    for (func_t *f = __fini_array_end; f && f != __fini_array_start;)
        (*--f)();
}

__attribute__((visibility("default"), noreturn))
void _start(void *process_parameters, func_t loader_teardown)
{
    id4_startup_log("CRT: _start entered");
    id4_log_number("CRT: _start runtime address ", (uintptr_t)&_start);
    if (!process_parameters) {
        id4_startup_log("CRT: loader supplied null process parameters");
        __builtin_trap();
    }
    const int argc = *(const int *)process_parameters;
    char **argv = (char **)((uint8_t *)process_parameters + sizeof(uint64_t));

    ps5_fp_ieee();
    id4_startup_log("CRT: before _init_env");
    _init_env(process_parameters);
    id4_startup_log("CRT: _init_env completed");
    if (loader_teardown)
        (void)atexit(loader_teardown);
    (void)atexit(_fini);
    _init();
    id4_startup_log("CRT: entering main");
    const int status = main(argc, argv);
    id4_log_number("CRT: main returned ", (uint32_t)status);
    catchReturnFromMain(status);
    /* exit() → signal 12 on PS5 native titles; use sceSystemServiceLoadExec */
    sceSystemServiceLoadExec("exit", NULL);
    for (;;) __asm__ volatile("" ::: "memory");
}
