/* unistd.h - minimal POSIX shim for MSVC. */
#ifndef LIND_UNISTD_SHIM_H
#define LIND_UNISTD_SHIM_H

#include <io.h>
#include <direct.h>
#include <windows.h>

#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#define access _access
#define chdir  _chdir
#define mkdir  _mkdir
#define rmdir  _rmdir
#define unlink _unlink
#define getcwd _getcwd
#define dup    _dup
#define dup2   _dup2
#define fileno _fileno
#define isatty _isatty
#define sleep(n) Sleep((n) * 1000u)

static unsigned int usleep(unsigned int usec)
{
    Sleep(usec / 1000u ? usec / 1000u : 1u);
    return 0;
}

static int getpid(void) { return (int)GetCurrentProcessId(); }
static int getuid(void) { return 0; }
static int geteuid(void) { return 0; }
static int getgid(void) { return 0; }
static int getegid(void) { return 0; }

#endif
