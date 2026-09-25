// sdlcastg - Android でだけ足す mbedTLS の設定（MBEDTLS_USER_CONFIG_FILE）
//
// mbedTLS は Linux では getrandom(2) で乱数の種を取る。古いカーネルの端末
// （XS17: MT6737M / Linux 3.18 / Android 8.1）では、これが 1 回に数十秒〜1 分半も
// 止まり、つなぐまでに 2 分以上かかった（2026-09-25）。/dev/urandom なら待たずに
// 読めるので、既定の取り方をやめて、/dev/urandom を読む mbedtls_hardware_poll
// （channel.cpp）を種にする。

#ifndef SDLCASTG_MBEDTLS_USER_CONFIG_H
#define SDLCASTG_MBEDTLS_USER_CONFIG_H

#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

#endif
