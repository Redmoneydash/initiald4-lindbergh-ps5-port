/* id4_diag.c — startup/fault logging: kernel debug out + append file.
 * Adapted from TheSimpsonsGame-PS5-Builder prospero/app_crt.cpp: writes
 * before libc init, so only sceKernel* calls and byte loops — no libc.
 * Log lands in /app0/logs when the image is writable, else the title
 * state dir /data/homebrew/PPSA99406/logs (EROFS/ENOENT fallback).
 */
#include <stdint.h>
#include <stddef.h>

extern int sceKernelOpen(const char *path, int flags, int mode);
extern int sceKernelMkdir(const char *path, int mode);
extern long sceKernelWrite(int fd, const void *data, size_t bytes);
extern int sceKernelFsync(int fd);
extern int sceKernelClose(int fd);
extern int sceKernelDebugOutText(int channel, const char *text);

#define O_WRONLY_PS5 1
#define O_CREAT_PS5  0x200
#define O_APPEND_PS5 0x8

/* FreeBSD errno in 0x8002xxxx native results. */
#define KROFS   (0x80020000u | 30)
#define KEXISTS (0x80020000u | 17)
#define KNOENT  (0x80020000u | 2)

static const char kAppRoot[]       = "/app0";
static const char kStateRoot[]     = "/data/homebrew/PPSA99406";
static const char kAppLogDir[]     = "/app0/logs";
static const char kAppLogPath[]    = "/app0/logs/startup.log";
static const char kTitleLogDir[]   = "/data/homebrew/PPSA99406/logs";
static const char kTitleLogPath[]  = "/data/homebrew/PPSA99406/logs/startup.log";

/* 0=unselected 1=app 2=title — fixed by the first write, like theirs. */
static int state_location;

static const char *log_dir(void)  { return state_location == 2 ? kTitleLogDir : kAppLogDir; }
static const char *log_path(void) { return state_location == 2 ? kTitleLogPath : kAppLogPath; }

const char *id4_state_root(void)
{
    return state_location == 2 ? kStateRoot : kAppRoot;
}

static size_t append(char *out, size_t at, size_t end, const char *text)
{
    if (text)
        while (at < end && *text)
            out[at++] = *text++;
    return at;
}

static size_t append_hex(char *out, size_t at, uint64_t value)
{
    static const char digits[] = "0123456789abcdef";
    out[at++] = '0'; out[at++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        out[at++] = digits[(value >> shift) & 15];
    return at;
}

static void emit_kernel(const char *text, size_t size)
{
    sceKernelDebugOutText(0, text);
    sceKernelWrite(2, text, size);
}

static void write_record(int fd, const char *text, size_t size)
{
    size_t written = 0;
    while (written < size) {
        const long r = sceKernelWrite(fd, text + written, size - written);
        if (r <= 0 || (size_t)r > size - written)
            break;
        written += (size_t)r;
    }
}

static void create_title_log_parents(void)
{
    const char *const parents[] = {"/data/homebrew", kStateRoot, kTitleLogDir};
    for (int i = 0; i < 3; i++)
        sceKernelMkdir(parents[i], 0777);
}

void id4_startup_log(const char *message)
{
    char text[1024];
    size_t size = append(text, 0, sizeof(text) - 2, "[PPSA99406] ");
    size = append(text, size, sizeof(text) - 2, message);
    if (size == 0 || text[size - 1] != '\n')
        text[size++] = '\n';
    text[size] = '\0';
    emit_kernel(text, size);

    const int first_open = state_location == 0;
    if (first_open)
        state_location = 1;
    const int mkdir_result = sceKernelMkdir(log_dir(), 0777);
    int fd = sceKernelOpen(log_path(), O_WRONLY_PS5 | O_CREAT_PS5 | O_APPEND_PS5, 0666);
    const int readonly_open = (uint32_t)fd == KROFS;
    const int missing_ro_dir =
        (uint32_t)mkdir_result == KROFS && (uint32_t)fd == KNOENT;
    if (first_open && (readonly_open || missing_ro_dir)) {
        state_location = 2;
        create_title_log_parents();
        fd = sceKernelOpen(log_path(), O_WRONLY_PS5 | O_CREAT_PS5 | O_APPEND_PS5, 0666);
    }
    if (fd < 0)
        return;
    write_record(fd, text, size);
    sceKernelFsync(fd);
    sceKernelClose(fd);
}

/* Per-initializer and progress marks: klog only, so hundreds of Mesa
 * constructors don't fsync per line. */
void id4_log_number(const char *prefix, uint64_t value)
{
    char text[160];
    size_t at = append(text, 0, 130, "[PPSA99406] ");
    at = append(text, at, 130, prefix);
    at = append_hex(text, at, value);
    text[at++] = '\n';
    text[at] = '\0';
    sceKernelDebugOutText(0, text);
}

/* Periodic heartbeat — prospero_metrics equivalent. A hang leaves the last
 * beat in startup.log; delta between beats shows where time stopped. */
extern int pthread_create(void *, void *, void *(*)(void *), void *);
extern unsigned int sleep(unsigned int);

static void *metrics_run(void *unused)
{
    (void)unused;
    for (unsigned beat = 1;; beat++) {
        sleep(15);
        char text[96];
        size_t at = append(text, 0, 80, "metrics: alive beat ");
        /* small decimal, avoid snprintf here to keep the handler light */
        char num[24]; int n = 0; unsigned v = beat;
        do { num[n++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (n) text[at++] = num[--n];
        text[at] = '\0';
        id4_startup_log(text);
    }
    return (void *)0;
}

void id4_metrics_start(void)
{
    /* pthread_t is opaque; any storage works. */
    static unsigned long thread;
    pthread_create(&thread, (void *)0, metrics_run, (void *)0);
}
