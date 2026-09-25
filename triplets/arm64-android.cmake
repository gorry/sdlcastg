# sdlcastg 用の Android の triplet（vcpkg の --overlay-triplets で使う。BUILD.md）
#
# vcpkg の標準のものとの違い:
#   - API 21（Android 5.0）で作る。標準は 28 で、それより古い端末で読み込めなくなる
#   - C++ の実行時ライブラリは静的（c++_static。アプリ側も同じにする）
#   - vcpkg の Android 用ツールチェーンを明示する（NDK は環境変数 ANDROID_NDK_HOME）
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE static)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 21)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=arm64-v8a)
set(VCPKG_CHAINLOAD_TOOLCHAIN_FILE "${CMAKE_CURRENT_LIST_DIR}/android-toolchain.cmake")
set(VCPKG_ENV_PASSTHROUGH ANDROID_NDK_HOME)
