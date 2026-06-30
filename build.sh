#!/bin/bash
# build.sh VERSION 8
# Usage:
#   ./build.sh          # defaults to dev → build/main-dev (linux) + build/main-dev-x64.exe (windows)
#   ./build.sh dev      # same as above
#   ./build.sh live     # → build/main-live + build/main-live-x64.exe
set -e

# -------- Channel selection --------
CHANNEL="${1:-dev}"

EXTRA_FLAGS=""
if [[ -f "cflags.txt" ]]; then
    while read -r key rest; do
        [[ -z "$key" ]] && continue
        [[ "$key" == \#* ]] && continue
        if [[ "$key" == "$CHANNEL" ]]; then
            EXTRA_FLAGS="$rest"
            break
        fi
    done < "cflags.txt"
fi

if [[ -z "$EXTRA_FLAGS" ]]; then
    echo "Unknown channel '$CHANNEL', falling back to 'dev'."
    CHANNEL="dev"
    sleep 1
    if [[ -f "cflags.txt" ]]; then
        while read -r key rest; do
            [[ -z "$key" ]] && continue
            [[ "$key" == \#* ]] && continue
            if [[ "$key" == "dev" ]]; then
                EXTRA_FLAGS="$rest"
                break
            fi
        done < "cflags.txt"
    fi
    if [[ -z "$EXTRA_FLAGS" ]]; then
        echo "No flags found for 'dev' in cflags.txt, continuing without extra flags."
    fi
fi

# -------- Compiler / paths --------
CXX_LINUX=g++
CXX_WIN=x86_64-w64-mingw32-g++
CXXFLAGS="-std=c++20 -Wall -Wextra ${EXTRA_FLAGS}"

SRC="$(find src -name '*.cpp' -print)"
OUT_LINUX="build/main-${CHANNEL}"
OUT_WIN="build/main-${CHANNEL}-x64.exe"

INCLUDES=""
for dir in modules/*/include; do
    [ -d "$dir" ] && INCLUDES="$INCLUDES -I$dir"
done
for dir in libs/*/include; do
    [ -d "$dir" ] && INCLUDES="$INCLUDES -I$dir"
done

MODULE_SOURCES=""
for file in modules/*/src/*.cpp; do
    [ -f "$file" ] && MODULE_SOURCES="$MODULE_SOURCES $file"
done

# -------- Linux libs (skip .dll.a import libs) --------
LINKS_LINUX=""
for lib in libs/*/lib/*.a libs/*/lib/*.so; do
    [[ "$lib" == *.dll.a ]] && continue
    [ -f "$lib" ] && LINKS_LINUX="$LINKS_LINUX $lib"
done

# -------- Windows libs --------
LINKS_WIN=""
WIN_L_FLAGS=""
for dir in libs/*/lib; do
    [ -d "$dir" ] && WIN_L_FLAGS="$WIN_L_FLAGS -L$dir"
done
for dir in libs/*/win; do
    [ -d "$dir" ] && WIN_L_FLAGS="$WIN_L_FLAGS -L$dir"
done

for lib in libs/*/lib/*.dll.a libs/*/win/*.dll.a; do
    if [ -f "$lib" ]; then
        base=$(basename "$lib" .dll.a)
        name="${base#lib}"
        LINKS_WIN="$LINKS_WIN -l$name"
    fi
done
for lib in libs/*/win/*.a libs/*/win/*.lib; do
    [ -f "$lib" ] && LINKS_WIN="$LINKS_WIN $lib"
done

# -------- Windows system libs --------
SDL3_WIN_SYS="-lmingw32 -mwindows -lm -ldinput8 -ldxguid -ldxerr8 \
              -luser32 -lgdi32 -lwinmm -limm32 -lole32 -loleaut32 \
              -lshell32 -lsetupapi -lversion -luuid"

mkdir -p build

# -------- Clean build folder (executables, .dll, .so only) --------
echo "Cleaning build/..."
find build/ -maxdepth 1 -type f ! -name "*.*" -delete
find build/ -maxdepth 1 -type f \( -name "*.exe" -o -name "*.dll" -o -name "*.so" \) -delete
echo "Done."
echo ""

echo "Channel:   $CHANNEL"
echo "CXXFLAGS:  $CXXFLAGS"
echo "Sources:   $SRC $MODULE_SOURCES"
echo "Includes:  $INCLUDES"
echo ""

# -------- Sync clone/ into build/ --------
if [ -d "clone" ]; then
    cp -r clone/. build/
fi

# -------- Linux build --------
echo ">>> Linux build -> $OUT_LINUX"
if command -v "$CXX_LINUX" &>/dev/null; then
    $CXX_LINUX $CXXFLAGS $SRC $MODULE_SOURCES $INCLUDES $LINKS_LINUX -o "$OUT_LINUX"
    echo "    OK"
else
    echo "    SKIPPED — $CXX_LINUX not found"
fi

echo ""

# -------- Windows build --------
echo ">>> Windows build -> $OUT_WIN"
if command -v "$CXX_WIN" &>/dev/null; then
    $CXX_WIN $CXXFLAGS $SRC $MODULE_SOURCES \
        $INCLUDES \
        $WIN_L_FLAGS $LINKS_WIN \
        $SDL3_WIN_SYS \
        -static-libgcc -static-libstdc++ \
        -o "$OUT_WIN"
    echo "    OK"

    # Copy all .dll files from libs into build/
    echo "    Copying DLLs..."
    found_dlls=0
    for dll in libs/*/bin/*.dll libs/*/win/*.dll libs/*/*.dll; do
        if [ -f "$dll" ]; then
            cp "$dll" build/
            echo "      $dll -> build/$(basename "$dll")"
            found_dlls=1
        fi
    done

    # Copy MinGW runtime DLLs
    for dll in \
        /usr/x86_64-w64-mingw32/lib/libwinpthread-1.dll \
        /usr/lib/gcc/x86_64-w64-mingw32/*/libwinpthread-1.dll \
        /usr/lib/gcc/x86_64-w64-mingw32/*/libgcc_s_seh-1.dll \
        /usr/lib/gcc/x86_64-w64-mingw32/*/libstdc++-6.dll; do
        if [ -f "$dll" ]; then
            cp "$dll" build/
            echo "      $dll -> build/$(basename "$dll")"
            found_dlls=1
        fi
    done

    if [[ $found_dlls -eq 0 ]]; then
        echo "      WARNING: No .dll files found — Windows exe may not run"
    fi
else
    echo "    SKIPPED — $CXX_WIN not found (install mingw-w64)"
fi

echo ""
echo "Build complete."
sleep 1
clear
echo "=== START OF OUTPUT ==="
echo ""
./"$OUT_LINUX"