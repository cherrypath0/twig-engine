@echo off
setlocal enabledelayedexpansion

rem ---- Channel selection ----

set "CHANNEL=%1"
if "%CHANNEL%"=="" set "CHANNEL=dev"

rem Default extra flags (if not found in cflags.txt)
set "EXTRA_FLAGS="

rem Read cflags.txt to find flags for this channel
if exist "cflags.txt" (
    for /f "usebackq tokens=1,* delims= " %%A in ("cflags.txt") do (
        rem Skip comments and empty lines
        if not "%%A"=="" if /i not "%%A"=="#" (
            if /i "%%A"=="%CHANNEL%" (
                set "EXTRA_FLAGS=%%B"
            )
        )
    )
)

if "%EXTRA_FLAGS%"=="" (
    echo Unknown channel "%CHANNEL%", falling back to "dev".
    set "CHANNEL=dev"
    rem Try again for dev
    set "EXTRA_FLAGS="
    if exist "cflags.txt" (
        for /f "usebackq tokens=1,* delims= " %%A in ("cflags.txt") do (
            if not "%%A"=="" if /i not "%%A"=="#" (
                if /i "%%A"=="dev" (
                    set "EXTRA_FLAGS=%%B"
                )
            )
        )
    )
    if "%EXTRA_FLAGS%"=="" (
        echo No flags found for "dev" in cflags.txt, continuing without extra flags.
    )
)

rem ---- Compiler / paths ----

set "CXX=g++"
set "CXXFLAGS=-std=c++20 -Wall -Wextra %EXTRA_FLAGS%"

set "SRC=src\main.cpp"
set "OUT=build\main-%CHANNEL%.exe"

rem Collect include paths from modules\*\include and libs\*\include
set "INCLUDES="

for /d %%D in (modules\*) do (
    if exist "%%D\include" (
        set "INCLUDES=!INCLUDES! -I%%D\include"
    )
)

for /d %%D in (libs\*) do (
    if exist "%%D\include" (
        set "INCLUDES=!INCLUDES! -I%%D\include"
    )
)

rem Collect module sources modules\*\src\*.cpp
set "MODULE_SOURCES="

for /r "modules" %%F in (*.cpp) do (
    set "MODULE_SOURCES=!MODULE_SOURCES! %%F"
)

rem Collect static and shared libraries from libs\*\lib\*.a / *.so / *.lib / *.dll
set "LINKS="

for /r "libs" %%F in (*.a) do (
    set "LINKS=!LINKS! %%F"
)
for /r "libs" %%F in (*.so) do (
    set "LINKS=!LINKS! %%F"
)
for /r "libs" %%F in (*.lib) do (
    set "LINKS=!LINKS! %%F"
)
for /r "libs" %%F in (*.dll) do (
    rem Usually you don't link DLLs directly, but include in case your toolchain allows it
    set "LINKS=!LINKS! %%F"
)

rem Create build directory if needed
if not exist "build" (
    mkdir "build"
)

echo Channel:   %CHANNEL%
echo Output:    %OUT%
echo CXXFLAGS:  %CXXFLAGS%
echo Sources:   %SRC% %MODULE_SOURCES%
echo Includes:  %INCLUDES%
echo Links:     %LINKS%
echo.

rem Compile
%CXX% %CXXFLAGS% %SRC% %MODULE_SOURCES% %INCLUDES% %LINKS% -o "%OUT%"
if errorlevel 1 (
    echo Build failed.
    goto :EOF
)

echo.
echo Build successful -> %OUT%
echo.
echo === START OF OUTPUT ===
echo.

"%OUT%"

endlocal