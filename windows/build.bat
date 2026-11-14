@echo off
rem build.bat - Windows (x86, MSVC) build of the Initial D 4 recomp.
rem
rem   build.bat <gen_dir>
rem
rem <gen_dir> holds the lifter output for YOUR id4.elf - recomp_funcs_*.c,
rem recomp_funcs_list.h, recomp_imports.h, recomp_plt.h, recomp_symbols.h.
rem None of that is in this repo (it is lifted game code). Output: id4.exe
rem next to this script. Needs the VS 2022 Build Tools (x86 toolset).
setlocal
if "%~1"=="" (echo usage: build.bat ^<gen_dir^> & exit /b 2)
set GEN=%~f1
if not exist "%GEN%\recomp_imports.h" (echo %GEN% has no recomp_imports.h & exit /b 2)

set VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat
if not exist "%VCVARS%" set VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat
call "%VCVARS%" x86 >nul || (echo vcvarsall not found & exit /b 1)

set HERE=%~dp0
set OBJ=%HERE%obj
if not exist "%OBJ%" mkdir "%OBJ%"
rem 32-bit on purpose: guest pointers are host pointers, so the process must
rem live in a 4 GB address space.
set CFLAGS=/nologo /O1 /Gy /GF /GS- /W1 /wd4244 /wd4267 /wd4018 /wd4090 /wd4996 /FIwin_compat.h /I"%HERE%." /I"%GEN%"

cl %CFLAGS% /MP /c "%HERE%guest.c" "%HERE%dispatch.c" "%HERE%syscall.c" "%HERE%hle.c" ^
   "%HERE%hle_libc.c" "%HERE%hle_cxx.c" "%HERE%hle_gl.c" "%HERE%hle_glut.c" "%HERE%hle_matrix.c" ^
   "%HERE%hle_pthread.c" "%HERE%hle_sega.c" "%HERE%hle_vidmode.c" "%HERE%hle_window.c" ^
   "%HERE%hle_cg.c" "%HERE%crash_report.c" "%HERE%overrides.c" "%HERE%host.c" "%HERE%win_stubs.c" ^
   /Fo"%OBJ%\\" || goto :fail

rem lifted code (~90 TUs, ~200 MB of C) - /MP spreads it over every core
cl %CFLAGS% /MP /c "%GEN%\recomp_funcs_*.c" /Fo"%OBJ%\\" || goto :fail

link /nologo /OUT:"%HERE%id4.exe" /SUBSYSTEM:CONSOLE "%OBJ%\*.obj" ^
     opengl32.lib user32.lib gdi32.lib kernel32.lib || goto :fail
echo BUILD_OK: %HERE%id4.exe
exit /b 0
:fail
echo BUILD_FAILED
exit /b 1
