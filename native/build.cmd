@echo off
rem Builds Murmur with MinGW-w64 (g++ and mingw32-make on PATH, or in C:\mingw64\bin).
setlocal
set PATH=C:\mingw64\bin;%PATH%
cd /d "%~dp0"
mingw32-make -j %NUMBER_OF_PROCESSORS% %*
if errorlevel 1 exit /b 1
echo.
echo Built bin\Murmur.exe. Run "bin\Murmur.exe --install" to add it to the Start menu.
