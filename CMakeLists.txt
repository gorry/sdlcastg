# sdlcastg - SDL2 の映像と音声を Google Cast 対応機器（Chromecast など）へ送るライブラリ
#
# 単独でも（見本プログラム castplay などを含めて）ビルドでき、アプリからは
# add_subdirectory で取り込む（README.md）。
#
#   cmake -S . -B build/win64 -A x64
#   cmake --build build/win64 --config Release
#
# 依存（BUILD.md）。置き場所は下の変数で差し替えられる。取り込む側は
# add_subdirectory の前に普通の変数として決めておけばよい。
#   SDLCASTG_MBEDTLS_DIR          mbedTLS 3.6 のソース（一緒にビルドする）
#                                 既定: third_party/mbedtls-3.6.7
#   SDLCASTG_VCPKG_INSTALLED_DIR  vcpkg で作った libvpx / opus / libyuv の入れ先
#                                 （triplet ごとのフォルダの親）
#                                 既定: third_party/vcpkg_installed
#   SDLCASTG_DEPS_DIR             上の <入れ先>/<triplet>。直に指すときに使う
#   SDLCASTG_SDL2_ROOT            SDL2 の VC 開発用パッケージ（Windows で、取り込む側に
#                                 sdl2 ターゲットが無いとき）。既定: third_party/SDL2-2.32.10
#
# SDL2 は SDL の層（sdlcastg_sdl）と見本 sdlcastgdemo だけが使う。取り込む側に
# sdl2 という名前のターゲットがあればそれを使う。

cmake_minimum_required(VERSION 3.20)
project(sdlcastg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 見本は、単独でビルドするときだけ既定で作る（取り込まれたときは作らない）。
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
	set(_sdlcastg_top ON)
else()
	set(_sdlcastg_top OFF)
endif()
option(SDLCASTG_BUILD_TOOLS "Build the sample programs (castplay, sdlcastgdemo, sdlcastg_encodetest)"
       ${_sdlcastg_top})

# 置き場所の既定。取り込む側が普通の変数で決めていれば、そちらを使う。
if(NOT DEFINED SDLCASTG_MBEDTLS_DIR)
	set(SDLCASTG_MBEDTLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls-3.6.7"
	    CACHE PATH "mbedTLS 3.6 source tree")
endif()
if(NOT DEFINED SDLCASTG_VCPKG_INSTALLED_DIR)
	set(SDLCASTG_VCPKG_INSTALLED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/vcpkg_installed"
	    CACHE PATH "vcpkg install root that holds libvpx / opus / libyuv (one folder per triplet)")
endif()
if(NOT DEFINED SDLCASTG_SDL2_ROOT)
	set(SDLCASTG_SDL2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/SDL2-2.32.10"
	    CACHE PATH "SDL2 development package for Visual C++ (Windows only)")
endif()

# ---- 版（Profile.ini。著作者専用） ------------------------------------------
# 値は Profile.ini の [Version] にだけあり、ビルド時に src/sdlcastg_version.h.in →
# <ビルド先>/generated/sdlcastg_version.h へ写す。形は "2026.0925.1" と 202609251。
set(SDLCASTG_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/Profile.ini")
set(SDLCASTG_VERSION_TEXT "unknown")
set(SDLCASTG_VERSION_NUMBER 0)
if(EXISTS "${SDLCASTG_PROFILE}")
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SDLCASTG_PROFILE}")
	file(READ "${SDLCASTG_PROFILE}" _sdlcastg_profile_text)
	# 行に割る。";" は CMake のリストの区切りなので先に守る（コメント行に出る）。
	string(REPLACE ";" "\\;" _sdlcastg_profile_text "${_sdlcastg_profile_text}")
	string(REGEX REPLACE "\r?\n" ";" _sdlcastg_profile_lines "${_sdlcastg_profile_text}")
	function(sdlcastg_profile_get section key outvar)
		set(cur "")
		set(found "")
		foreach(line IN LISTS _sdlcastg_profile_lines)
			if(line MATCHES "^[ \t]*\\[([^]]+)\\]")
				set(cur "${CMAKE_MATCH_1}")
			elseif(line MATCHES "^[ \t]*[;#]")
				# コメント
			elseif(cur STREQUAL "${section}" AND line MATCHES "^[ \t]*${key}[ \t]*=(.*)$")
				string(STRIP "${CMAKE_MATCH_1}" found)
			endif()
		endforeach()
		if(found STREQUAL "")
			message(FATAL_ERROR "sdlcastg/Profile.ini: [${section}] ${key} がありません")
		endif()
		set(${outvar} "${found}" PARENT_SCOPE)
	endfunction()
	sdlcastg_profile_get(Version Text SDLCASTG_VERSION_TEXT)
	sdlcastg_profile_get(Version Number SDLCASTG_VERSION_NUMBER)
else()
	message(WARNING "sdlcastg: Profile.ini が無いので、版は \"unknown\" にする")
endif()
message(STATUS "sdlcastg: ${SDLCASTG_VERSION_TEXT}")
configure_file(src/sdlcastg_version.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/sdlcastg_version.h"
               @ONLY)

# ---- mbedTLS ------------------------------------------------------------------
if(NOT EXISTS "${SDLCASTG_MBEDTLS_DIR}/CMakeLists.txt")
	message(FATAL_ERROR "mbedTLS が見つかりません: ${SDLCASTG_MBEDTLS_DIR}（BUILD.md を参照）")
endif()

# mbedTLS はライブラリだけを静的に作る。third_party は無改変で置くだけなので、
# 警告を誤りにはしない。
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory("${SDLCASTG_MBEDTLS_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/mbedtls" EXCLUDE_FROM_ALL)
# Android では乱数の種を /dev/urandom から取る（src/mbedtls_user_config.h）。
# 構造体の形が変わるので、mbedTLS を使う側にも同じ設定が要る（PUBLIC）。
if(ANDROID)
	target_compile_definitions(mbedcrypto PUBLIC
		"MBEDTLS_USER_CONFIG_FILE=\"${CMAKE_CURRENT_SOURCE_DIR}/src/mbedtls_user_config.h\"")
endif()

# ---- libvpx と opus と libyuv（vcpkg）。Debug では debug/lib のものを使う -------
if(WIN32)
	set(_sdlcastg_triplet "x64-windows-static-md")
elseif(ANDROID)
	if(ANDROID_ABI STREQUAL "arm64-v8a")
		set(_sdlcastg_triplet "arm64-android")
	elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
		set(_sdlcastg_triplet "arm-neon-android")
	else()
		set(_sdlcastg_triplet "x64-android")
	endif()
else()
	set(_sdlcastg_triplet "x64-linux")
endif()
if(NOT DEFINED SDLCASTG_DEPS_DIR)
	set(SDLCASTG_DEPS_DIR "${SDLCASTG_VCPKG_INSTALLED_DIR}/${_sdlcastg_triplet}")
endif()
foreach(_lib vpx opus yuv)
	find_library(SDLCASTG_${_lib}_RELEASE NAMES ${_lib} lib${_lib}
	             PATHS "${SDLCASTG_DEPS_DIR}/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
	find_library(SDLCASTG_${_lib}_DEBUG NAMES ${_lib} lib${_lib} ${_lib}d
	             PATHS "${SDLCASTG_DEPS_DIR}/debug/lib" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
	if(NOT SDLCASTG_${_lib}_RELEASE)
		message(FATAL_ERROR "${_lib} が見つかりません: ${SDLCASTG_DEPS_DIR}/lib（BUILD.md を参照）")
	endif()
	if(NOT SDLCASTG_${_lib}_DEBUG)
		set(SDLCASTG_${_lib}_DEBUG "${SDLCASTG_${_lib}_RELEASE}")
	endif()
endforeach()

# ---- 本体 -------------------------------------------------------------------
add_library(sdlcastg STATIC
	src/castmsg.cpp
	src/channel.cpp
	src/discovery.cpp
	src/encoder.cpp
	src/httpserver.cpp
	src/json.cpp
	src/livesource.cpp
	src/net.cpp
	src/sdlcastg.cpp
	src/session.cpp
	src/webm.cpp
	include/sdlcastg.h
	src/castmsg.h
	src/channel.h
	src/discovery.h
	src/encoder.h
	src/httpserver.h
	src/json.h
	src/livesource.h
	src/net.h
	src/session.h
	src/webm.h
)
target_include_directories(sdlcastg PUBLIC include "${CMAKE_CURRENT_BINARY_DIR}/generated"
                           PRIVATE src)
# third_party のヘッダーの警告は出さない（MSVC では /external:I になる）。
target_include_directories(sdlcastg SYSTEM PRIVATE "${SDLCASTG_DEPS_DIR}/include" "${SDLCASTG_DEPS_DIR}/include/opus")
target_link_libraries(sdlcastg PRIVATE mbedtls mbedx509 mbedcrypto
	optimized "${SDLCASTG_vpx_RELEASE}" debug "${SDLCASTG_vpx_DEBUG}"
	optimized "${SDLCASTG_opus_RELEASE}" debug "${SDLCASTG_opus_DEBUG}"
	optimized "${SDLCASTG_yuv_RELEASE}" debug "${SDLCASTG_yuv_DEBUG}")
if(WIN32)
	target_link_libraries(sdlcastg PRIVATE ws2_32 iphlpapi)
	target_compile_definitions(sdlcastg PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
endif()
if(MSVC)
	target_compile_options(sdlcastg PRIVATE /W4 /utf-8)
else()
	target_compile_options(sdlcastg PRIVATE -Wall -Wextra)
	find_package(Threads REQUIRED)
	target_link_libraries(sdlcastg PUBLIC Threads::Threads)
endif()

# ---- SDL の層 -----------------------------------------------------------------
# 取り込む側の sdl2 ターゲットがあればそれを使う。無ければ、Windows は SDL2 の
# VC 開発用パッケージ、それ以外は find_package(SDL2) で用意する。
if(NOT TARGET sdl2 AND WIN32 AND EXISTS "${SDLCASTG_SDL2_ROOT}/include/SDL.h")
	add_library(sdl2 INTERFACE)
	target_include_directories(sdl2 INTERFACE "${SDLCASTG_SDL2_ROOT}/include")
	target_link_libraries(sdl2 INTERFACE "${SDLCASTG_SDL2_ROOT}/lib/x64/SDL2.lib"
	                      "${SDLCASTG_SDL2_ROOT}/lib/x64/SDL2main.lib")
	set(SDLCASTG_SDL2_DLL "${SDLCASTG_SDL2_ROOT}/lib/x64/SDL2.dll")
endif()
if(NOT TARGET sdl2 AND NOT ANDROID)
	find_package(SDL2 CONFIG QUIET)
	if(TARGET SDL2::SDL2)
		add_library(sdl2 INTERFACE)
		target_link_libraries(sdl2 INTERFACE SDL2::SDL2)
	endif()
endif()
if(TARGET sdl2)
	add_library(sdlcastg_sdl STATIC src/sdlcastg_sdl.cpp include/sdlcastg_sdl.h)
	target_include_directories(sdlcastg_sdl PUBLIC include)
	target_link_libraries(sdlcastg_sdl PUBLIC sdlcastg sdl2)
	if(MSVC)
		target_compile_options(sdlcastg_sdl PRIVATE /W4 /utf-8)
	endif()
else()
	message(STATUS "sdlcastg: SDL2 が無いので、SDL の層（sdlcastg_sdl）は作らない")
endif()

# ---- 見本 ---------------------------------------------------------------------
if(SDLCASTG_BUILD_TOOLS)
	add_executable(castplay tools/castplay.cpp)
	target_link_libraries(castplay PRIVATE sdlcastg)
	if(MSVC)
		target_compile_options(castplay PRIVATE /W4 /utf-8)
		target_compile_definitions(castplay PRIVATE _CRT_SECURE_NO_WARNINGS)
	endif()

	# エンコードと WebM の書き出しの確かめ（受信側なし）。内部のクラスを直に使う。
	add_executable(sdlcastg_encodetest tools/encodetest.cpp)
	target_include_directories(sdlcastg_encodetest PRIVATE src)
	target_link_libraries(sdlcastg_encodetest PRIVATE sdlcastg)
	if(MSVC)
		target_compile_options(sdlcastg_encodetest PRIVATE /W4 /utf-8)
		target_compile_definitions(sdlcastg_encodetest PRIVATE _CRT_SECURE_NO_WARNINGS)
	endif()

	if(TARGET sdlcastg_sdl)
		add_executable(sdlcastgdemo tools/sdlcastgdemo.cpp)
		target_link_libraries(sdlcastgdemo PRIVATE sdlcastg_sdl)
		if(MSVC)
			target_compile_options(sdlcastgdemo PRIVATE /W4 /utf-8)
			target_compile_definitions(sdlcastgdemo PRIVATE _CRT_SECURE_NO_WARNINGS)
			# コンソールの見本なので main から始める（SDL2main は使わない）。
			target_compile_definitions(sdlcastgdemo PRIVATE SDL_MAIN_HANDLED)
		endif()
		if(SDLCASTG_SDL2_DLL)
			add_custom_command(TARGET sdlcastgdemo POST_BUILD
				COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SDLCASTG_SDL2_DLL}"
				        "$<TARGET_FILE_DIR:sdlcastgdemo>")
		endif()
	endif()
endif()
