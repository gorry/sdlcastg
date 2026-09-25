# sdlcastg/triplets の Android 用の triplet が使うツールチェーン。vcpkg の
# Android 用のものをそのまま読む。vcpkg の場所は、ここを読み込む側
# （<vcpkg>/scripts/buildsystems/vcpkg.cmake）の場所から求める。
get_filename_component(_sdlcastg_vcpkg_scripts "${CMAKE_PARENT_LIST_FILE}" DIRECTORY)
get_filename_component(_sdlcastg_vcpkg_scripts "${_sdlcastg_vcpkg_scripts}" DIRECTORY)
include("${_sdlcastg_vcpkg_scripts}/toolchains/android.cmake")
