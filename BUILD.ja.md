# sdlcastg のビルド

[English](BUILD.md)

## 用意するもの

- CMake 3.20 以降と、C++11 のコンパイラ
  - Windows: Visual Studio 2022（x64）
  - Android: NDK r28（28.2.13676358 で確認）、Ninja
- libvpx・Opus・libyuv を作るための [vcpkg](https://github.com/microsoft/vcpkg)
- 下のコマンドを打つ Git Bash など POSIX のシェル（PowerShell でも少し直せば使えます）

第三者のコードはこのリポジトリに入っていません。既定では `third_party/` の下を探します。

```
sdlcastg/
    third_party/
        mbedtls-3.6.7/      TLS（sdlcastg と一緒にソースからビルド）
        vcpkg_installed/    libvpx・Opus・libyuv（vcpkg が作る。triplet ごとのフォルダ）
        SDL2-2.32.10/       Visual C++ 用の SDL2（Windows。SDL の層と見本に使う）
```

置き場所は、どれも CMake の変数で変えられます（[CMake の変数](#cmake-の変数)）。

## mbedTLS 3.6.7

<https://github.com/Mbed-TLS/mbedtls/releases/tag/mbedtls-3.6.7> から **`mbedtls-3.6.7.tar.bz2`** を取ってきて、`third_party/` へ展開します。生成済みのファイルが入ったリリース版を使ってください（git のクローンには入っていません）。同じページの `mbedtls-3.6.7-sha256sum.txt` で照合できます（`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`）。

```sh
tar --force-local -xjf mbedtls-3.6.7.tar.bz2 -C third_party
```

sdlcastg は静的ライブラリだけを作り、ソースは書き換えません。Android では `MBEDTLS_USER_CONFIG_FILE`（`src/mbedtls_user_config.h`）を渡し、乱数の種を `/dev/urandom` から取らせます。

## libvpx / Opus / libyuv（vcpkg）

libvpx は独自の configure で作るため、MSVC や NDK から直にはビルドしにくいので、3 つとも vcpkg で作ります。入れ先は `third_party/vcpkg_installed/` で、vcpkg 本体の `installed/` は汚しません。libvpx 1.16.0 / Opus 1.5.2 / libyuv 1916 で確かめています。libyuv に付いてくる libjpeg-turbo はリンクしません。

vcpkg は**クラシックモード**（`--classic`）で使います。マニフェストモードだと入れ先に triplet が 1 つしか置けず、Android 向けを入れた時点で Windows 向けが消えます。

```sh
# Android 向けを作るときだけ、NDK の場所を渡す（/ 区切りで）
export ANDROID_NDK_HOME=C:/path/to/android-ndk

vcpkg install --classic --x-install-root=third_party/vcpkg_installed \
    --overlay-triplets=triplets --overlay-ports=ports \
    libvpx:x64-windows-static-md opus:x64-windows-static-md libyuv:x64-windows-static-md \
    libvpx:arm64-android opus:arm64-android libyuv:arm64-android \
    libvpx:arm-neon-android opus:arm-neon-android libyuv:arm-neon-android
```

- Windows 向けは静的ライブラリです（CRT は動的の `/MD`）。
- `triplets/` は Android 向けの triplet です。vcpkg の標準のものとの違いは次の 3 つです。
  - **API 21** で作ります（標準は 28 で、それより古い端末では読み込めなくなります）。
  - C++ の実行時ライブラリを静的（`c++_static`）にします。アプリ側も同じにしてください。
  - NDK のツールチェーンを明示します（vcpkg 2025-06-20 では、標準の triplet だと NDK が見つかりませんでした）。
- `ports/libvpx/` は vcpkg の libvpx の移植を写し、**Android で NEON を使う**ように直したものです。標準のものは Android を `generic-gnu`（SIMD なしの C だけ）で作ります。直したところには `sdlcastg:` と書いてあります。vcpkg の libvpx の版が上がったら、写し直してください。

初回は msys2 や nasm も取ってくるので、10 分ほどかかります。

## SDL2

SDL2 が要るのは、SDL の層（`sdlcastg_sdl`）と `sdlcastgdemo` だけです。本体（`sdlcastg`）と `castplay` は使いません。次の順に探します。

1. sdlcastg を取り込んだプロジェクトの `sdl2` という名前のターゲット
2. Windows: Visual C++ 用の開発パッケージ（`SDL2-devel-2.32.10-VC.zip`）を展開した `third_party/SDL2-2.32.10/`
3. ほかの OS: `find_package(SDL2 CONFIG)`

どれも無ければ、SDL の層は作らずにその旨を出します。

## Windows

```sh
cmake -S . -B build/win64 -A x64
cmake --build build/win64 --config Release
build/win64/Release/castplay.exe --list
build/win64/Release/sdlcastgdemo.exe <IP か名前> 60
```

Debug ビルドだと、720p の映像のエンコードが実時間に追いつかないことがあります。機器へ流して確かめるときは Release にしてください。

`castplay` を初めて動かすと、Windows のファイアウォールが許可を求めてきます（mDNS の応答の受け取りと、機器が HTTP サーバーへ流れを取りに来るため）。拒否すると、機器が流れを取りに来られません。

## Android

アプリに組み込むときは、アプリの CMake から `add_subdirectory` で取り込みます（README）。SDL の層はアプリの `sdl2` ターゲットを使います。

単独でビルドする場合は次のとおりです（見本は `adb shell` で動かします）。

```sh
cmake -S . -B build/android-arm64 -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-21 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build/android-arm64
adb push build/android-arm64/castplay /data/local/tmp/
adb shell "cd /data/local/tmp && chmod 755 castplay && ./castplay --list"
```

32bit の ARM は `-DANDROID_ABI=armeabi-v7a -DANDROID_ARM_NEON=ON` です（triplet は `arm-neon-android`）。

Git Bash から adb に `/data/...` を渡すときは、`MSYS_NO_PATHCONV=1` を付けてください。付けないと、Git のフォルダの下のパスに書き換えられます。

## CMake の変数

| 変数 | 既定 | 意味 |
|---|---|---|
| `SDLCASTG_MBEDTLS_DIR` | `third_party/mbedtls-3.6.7` | mbedTLS のソース |
| `SDLCASTG_VCPKG_INSTALLED_DIR` | `third_party/vcpkg_installed` | vcpkg の入れ先（triplet のフォルダの親） |
| `SDLCASTG_DEPS_DIR` | `<入れ先>/<triplet>` | 1 つの triplet のフォルダを直に指す |
| `SDLCASTG_SDL2_ROOT` | `third_party/SDL2-2.32.10` | Visual C++ 用の SDL2（Windows） |
| `SDLCASTG_BUILD_TOOLS` | 単独なら ON、取り込まれたら OFF | 見本を作る |

sdlcastg を取り込むプロジェクトは、`add_subdirectory` の前に普通の変数として決めておけば使われます。

triplet は OS から決まります: `x64-windows-static-md`、`arm64-android`、`arm-neon-android`、`x64-android`、`x64-linux`（未確認）。

## 版

版（`2026.0925.1` の形）は、configure のときに `Profile.ini` から読みます。`<ビルド先>/generated/sdlcastg_version.h`（`SDLCASTG_VERSION`、`SDLCASTG_VERSION_NUMBER`）に書き出され、`SDLCastG_GetVersion()` でも返ります。
