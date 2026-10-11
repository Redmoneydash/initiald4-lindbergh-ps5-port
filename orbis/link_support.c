/* link_support.c — linker --wrap implementations the SDK doesn't ship.
 * Pattern from TheSimpsonsGame-PS5-Builder prospero/link_support.c:
 * run the platform's C++ thread_local destructor registry when a thread
 * exits explicitly via pthread_exit (start-routine return is already
 * covered by the SDK's threads.o wrapper), and capture the caller PC on
 * abort before the native signal machinery reports its own RIP.
 */
#include <stdint.h>

extern void ps5p_run_thread_destructors(void);
extern void __real_pthread_exit(void *value) __attribute__((noreturn));
extern void __real_abort(void) __attribute__((noreturn));
extern int sceKernelDebugOutText(int channel, const char *text);

__attribute__((visibility("hidden"), noreturn)) void
__wrap_pthread_exit(void *value)
{
    ps5p_run_thread_destructors();
    __real_pthread_exit(value);
}

__attribute__((visibility("hidden"), noreturn)) void
__wrap_abort(void)
{
    const uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    char text[96];
    static const char digits[] = "0123456789abcdef";
    const char prefix[] = "[abort] linked-code caller ";
    char *p = text;
    for (const char *s = prefix; *s; ) *p++ = *s++;
    for (int shift = 60; shift >= 0; shift -= 4)
        *p++ = digits[(caller >> shift) & 15];
    *p++ = '\n';
    *p = '\0';
    sceKernelDebugOutText(0, text);
    __real_abort();
}
