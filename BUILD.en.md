# Building sdlcastg

[日本語](BUILD.md)

## Requirements

- CMake 3.20 or later, and a C++11 compiler
  - Windows: Visual Studio 2022 (x64)
  - Android: NDK r28 (tested with 28.2.13676358), Ninja
- [vcpkg](https://github.com/microsoft/vcpkg) to build libvpx, Opus and libyuv
- Git Bash or another POSIX shell for the commands below (they work in PowerShell with small changes)

Third-party code is not in this repository. By default it is looked for under `third_party/`:

```
sdlcastg/
    third_party/
        mbedtls-3.6.7/      TLS (built from source together with sdlcastg)
        vcpkg_installed/    libvpx, Opus, libyuv (one folder per triplet, made by vcpkg)
        SDL2-2.32.10/       SDL2 for Visual C++ (Windows, for the SDL layer and the demo)
```

Every location can be changed with a CMake variable (see [CMake variables](#cmake-variables)).

## mbedTLS 3.6.7

Download **`mbedtls-3.6.7.tar.bz2`** from <https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7>. Use the release archive, which contains generated files; a git clone does not. Extract it into `third_party/`. You can check it against `mbedtls-3.6.7-sha256sum.txt` on the same page (`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`).

```sh
tar --force-local -xjf mbedtls-3.6.7.tar.bz2 -C third_party
```

sdlcastg builds only the static libraries and does not modify the sources. On Android it passes `MBEDTLS_USER_CONFIG_FILE` (`src/mbedtls_user_config.h`), so that entropy comes from `/dev/urandom`.

## libvpx / Opus / libyuv (vcpkg)

libvpx has its own configure script and is hard to build directly with MSVC or the NDK, so all three are built with vcpkg. They go into `third_party/vcpkg_installed/` and leave vcpkg's own `installed/` alone. Tested with libvpx 1.16.0, Opus 1.5.2 and libyuv 1916. libyuv pulls in libjpeg-turbo, which sdlcastg does not link.

Use vcpkg's **classic mode** (`--classic`). In manifest mode the install root holds only one triplet, so installing the Android libraries removes the Windows ones.

```sh
# only when building for Android: where the NDK is (use / as separator)
export ANDROID_NDK_HOME=C:/path/to/android-ndk

vcpkg install --classic --x-install-root=third_party/vcpkg_installed \
    --overlay-triplets=triplets --overlay-ports=ports \
    libvpx:x64-windows-static-md opus:x64-windows-static-md libyuv:x64-windows-static-md \
    libvpx:arm64-android opus:arm64-android libyuv:arm64-android \
    libvpx:arm-neon-android opus:arm-neon-android libyuv:arm-neon-android
```

- Windows: static libraries with the dynamic CRT (`/MD`).
- `triplets/`: Android triplets. They differ from vcpkg's own in three ways:
  - They build for **API 21** (vcpkg's default is 28, which fails to load on older phones).
  - They use the static C++ runtime (`c++_static`). Your app must use it too.
  - They name the NDK toolchain explicitly. With vcpkg 2025-06-20, the stock triplets could not find the NDK.
- `ports/libvpx/`: a copy of vcpkg's libvpx port, changed to **use NEON on Android**. The stock port builds Android as `generic-gnu`, which is plain C with no SIMD. The changes are marked `sdlcastg:`. Copy the port again when vcpkg moves to a new libvpx.

The first run also downloads msys2 and nasm and takes about 10 minutes.

## SDL2

SDL2 is needed only for the SDL layer (`sdlcastg_sdl`) and `sdlcastgdemo`. The core library (`sdlcastg`) and `castplay` do not use it. sdlcastg finds SDL2 in this order:

1. A target named `sdl2` in the project that includes sdlcastg.
2. Windows: the Visual C++ development package (`SDL2-devel-2.32.10-VC.zip`) extracted to `third_party/SDL2-2.32.10/`.
3. Other platforms: `find_package(SDL2 CONFIG)`.

If none is found, the SDL layer is skipped with a message.

## Windows

```sh
cmake -S . -B build/win64 -A x64
cmake --build build/win64 --config Release
build/win64/Release/castplay.exe --list
build/win64/Release/sdlcastgdemo.exe <IP or name> 60
```

In a Debug build, the 720p video encoder may not keep up with real time. Use Release when streaming to a device.

The first run of `castplay` makes Windows Firewall ask for permission. It needs mDNS replies coming in, and the device fetching the stream from the HTTP server. If you deny it, the device never gets the stream.

## Android

As part of an app, add sdlcastg to the app's CMake with `add_subdirectory` (see README). The SDL layer uses the app's `sdl2` target.

For a standalone build (the samples run in an `adb shell`):

```sh
cmake -S . -B build/android-arm64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/android-arm64
adb push build/android-arm64/castplay /data/local/tmp/
adb shell "cd /data/local/tmp && chmod 755 castplay && ./castplay --list"
```

For 32-bit ARM, use `-DANDROID_ABI=armeabi-v7a -DANDROID_ARM_NEON=ON` (triplet `arm-neon-android`).

From Git Bash, set `MSYS_NO_PATHCONV=1` when passing `/data/...` to adb. Otherwise the path is rewritten to one under the Git installation.

## CMake variables

| Variable | Default | Meaning |
|---|---|---|
| `SDLCASTG_MBEDTLS_DIR` | `third_party/mbedtls-3.6.7` | mbedTLS source tree |
| `SDLCASTG_VCPKG_INSTALLED_DIR` | `third_party/vcpkg_installed` | vcpkg install root (the parent of the triplet folders) |
| `SDLCASTG_DEPS_DIR` | `<vcpkg installed>/<triplet>` | point directly at one triplet's folder |
| `SDLCASTG_SDL2_ROOT` | `third_party/SDL2-2.32.10` | SDL2 for Visual C++ (Windows) |
| `SDLCASTG_BUILD_TOOLS` | ON when built on its own, OFF when included | build the sample programs |

A project that includes sdlcastg can set these as normal variables before `add_subdirectory`.

The triplet is chosen from the platform: `x64-windows-static-md`, `arm64-android`, `arm-neon-android`, `x64-android`, or `x64-linux` (untested).

## Version

The version (for example `2026.0925.1`) is read from `Profile.ini` at configure time. It is written to `<build>/generated/sdlcastg_version.h` (`SDLCASTG_VERSION`, `SDLCASTG_VERSION_NUMBER`) and returned by `SDLCastG_GetVersion()`.
