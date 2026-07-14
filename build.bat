@echo off
rem MemGuard build script (no make needed) — run from the memguard folder.
setlocal
if not exist bin mkdir bin

echo Compiling memguard library...
gcc -std=c99 -Wall -Wextra -O2 -g -c src\memguard.c -o bin\memguard.o || goto :err

echo Compiling self-tests...
gcc -std=c99 -Wall -Wextra -O2 -g tests\test_memguard.c bin\memguard.o -o bin\test_memguard.exe || goto :err

echo Compiling demos...
for %%d in (demo_basic demo_leak demo_overflow demo_doublefree demo_fragmentation) do (
    gcc -std=c99 -Wall -Wextra -O2 -g demos\%%d.c bin\memguard.o -o bin\%%d.exe || goto :err
)

echo.
echo Build OK. Run bin\test_memguard.exe or any bin\demo_*.exe
exit /b 0

:err
echo BUILD FAILED
exit /b 1
