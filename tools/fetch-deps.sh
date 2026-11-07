#!/bin/sh
# Fetch and build what `make packages` needs into deps/: Zig for Linux and
# Windows, SDL2 built with Zig against glibc 2.31 and the SDL2 MinGW package,
# TinyCC (libtcc.so against glibc 2.31, libtcc.dll), AppImage's runtime and
# the GPL extra shaders. Every download is pinned by version and checksum.
# Parts already in deps/ are kept. The release workflow runs this; it works
# locally too.
#
#   tools/fetch-deps.sh
#
# Needs: curl, tar, unzip, git, cmake, ninja, make, python3, MinGW-w64
# (x86_64-w64-mingw32-gcc), and the X11/Wayland/audio/udev/dbus development
# headers, so SDL2 builds with those backends (it loads their libraries at
# run time, so the player's system supplies them).
set -e
cd "$(dirname "$0")/.."
mkdir -p deps/dl
deps=$PWD/deps

ZIG=0.17.0
ZIG_LNX_SHA=1cbe9df9f27e6b78d14ccbca43b6703a404ef79ef1c463de901d7f088d4e2026
ZIG_WIN_SHA=b5663f69581dcf391293fbf16c06cb80d81d806545ce618b4d0bab7f0eb8c428
SDL=2.32.10
SDL_SRC_SHA=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
SDL_MINGW_SHA=83a5d74012311edc3c0d40ea6faecbe57ad692aa033fa5dc273cc937e3938ff2
TCC_COMMIT=e23dabed154a8495f3ac1ff3cc9d04fa1c207893   # package.sh names it in SOURCE.txt
RUNTIME=20251108
RUNTIME_SHA=2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d

# fetch URL FILE SHA256: download into deps/dl unless already there, and check it
fetch() {
    [ -f "deps/dl/$2" ] || curl -fsSL --retry 3 -o "deps/dl/$2.part" "$1" && { [ -f "deps/dl/$2" ] || mv "deps/dl/$2.part" "deps/dl/$2"; }
    echo "$3  deps/dl/$2" | sha256sum -c --quiet || { echo "checksum mismatch: deps/dl/$2" >&2; exit 1; }
}

if [ ! -x deps/zig-linux/zig ]; then
    fetch https://ziglang.org/download/$ZIG/zig-x86_64-linux-$ZIG.tar.xz zig-x86_64-linux-$ZIG.tar.xz $ZIG_LNX_SHA
    tar -C deps -xJf deps/dl/zig-x86_64-linux-$ZIG.tar.xz
    rm -rf deps/zig-linux && mv deps/zig-x86_64-linux-$ZIG deps/zig-linux
fi
if [ ! -f deps/zig-windows/zig.exe ]; then
    fetch https://ziglang.org/download/$ZIG/zig-x86_64-windows-$ZIG.zip zig-x86_64-windows-$ZIG.zip $ZIG_WIN_SHA
    (cd deps && unzip -q dl/zig-x86_64-windows-$ZIG.zip)
    rm -rf deps/zig-windows && mv deps/zig-x86_64-windows-$ZIG deps/zig-windows
fi

# a private cache, so builds here don't fill the user's
export ZIG_GLOBAL_CACHE_DIR=$deps/zig-cache ZIG_LOCAL_CACHE_DIR=$deps/zig-cache

# SDL2 for Linux: Zig's clang against glibc 2.31, finding the system's
# X11/Wayland/audio headers after Zig's own
if [ ! -f deps/sdl2-lnx/build/libSDL2-2.0.so.0 ]; then
    fetch https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL2-$SDL.tar.gz SDL2-$SDL.tar.gz $SDL_SRC_SHA
    rm -rf deps/sdl2-lnx && mkdir -p deps/sdl2-lnx
    tar -C deps/sdl2-lnx -xzf deps/dl/SDL2-$SDL.tar.gz
    cat > deps/sdl2-lnx/zcc <<EOF
#!/bin/sh
exec "$deps/zig-linux/zig" cc -target x86_64-linux-gnu.2.31 -idirafter /usr/include -idirafter /usr/include/x86_64-linux-gnu "\$@"
EOF
    chmod +x deps/sdl2-lnx/zcc
    (cd deps/sdl2-lnx && CC=$PWD/zcc cmake -S SDL2-$SDL -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_RPATH=OFF -DCMAKE_LIBRARY_ARCHITECTURE=x86_64-linux-gnu > cfg.log 2>&1 \
        || { cat cfg.log; exit 1; })
    # the backends players need; each is loaded at run time if present
    for o in X11_SHARED WAYLAND_SHARED KMSDRM_SHARED OPENGL OPENGLES PULSEAUDIO_SHARED PIPEWIRE_SHARED ALSA_SHARED LIBUDEV HIDAPI DBUS; do
        grep -Eq "^--   SDL_$o +\(Wanted: ON\): ON" deps/sdl2-lnx/cfg.log || { echo "SDL2 would build without $o: install its development headers" >&2; exit 1; }
    done
    cmake --build deps/sdl2-lnx/build
fi
if [ ! -d deps/sdl2-mingw/SDL2-$SDL ]; then
    fetch https://github.com/libsdl-org/SDL/releases/download/release-$SDL/SDL2-devel-$SDL-mingw.tar.gz SDL2-devel-$SDL-mingw.tar.gz $SDL_MINGW_SHA
    rm -rf deps/sdl2-mingw && mkdir -p deps/sdl2-mingw
    tar -C deps/sdl2-mingw -xzf deps/dl/SDL2-devel-$SDL-mingw.tar.gz
fi

# TinyCC, unmodified, built twice from one checkout
if [ ! -f deps/tcc-lnx231/libtcc.so ] || [ ! -f deps/tcc-win/libtcc.dll ]; then
    [ -d deps/tinycc ] || git clone -q https://repo.or.cz/tinycc.git deps/tinycc
    git -C deps/tinycc checkout -q $TCC_COMMIT
fi
if [ ! -f deps/tcc-lnx231/libtcc.so ]; then
    rm -rf deps/tcc-lnx231 && cp -r deps/tinycc deps/tcc-lnx231
    (cd deps/tcc-lnx231 && ./configure --cc="$deps/zig-linux/zig cc -target x86_64-linux-gnu.2.31" > /dev/null && make -j"$(nproc)" libtcc.so)
fi
if [ ! -f deps/tcc-win/libtcc.dll ]; then
    rm -rf deps/tcc-win && cp -r deps/tinycc deps/tcc-win
    (cd deps/tcc-win && ./configure --cross-prefix=x86_64-w64-mingw32- --targetos=WIN32 --cpu=x86_64 --prefix="$PWD/inst" > /dev/null \
        && cc -DC2STR conftest.c -o c2str.exe \
        && make -j"$(nproc)" libtcc.dll)   # c2str runs during the build, so it's built for this machine
fi

# AppImage's static runtime (needs no libfuse2 on the player's system)
fetch https://github.com/AppImage/type2-runtime/releases/download/$RUNTIME/runtime-x86_64 runtime-x86_64-$RUNTIME $RUNTIME_SHA
mkdir -p deps/dl/appimage-runtime
cp deps/dl/runtime-x86_64-$RUNTIME deps/dl/appimage-runtime/runtime-x86_64

echo "deps ready"
