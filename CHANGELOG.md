# Changelog

Versions follow the form `YYYY.MMDD.N`.

## First release

- Discovery (mDNS), Cast V2 control over TLS, and the Default Media Receiver.
- Live streaming of RGBA video and PCM audio as WebM (VP8 + Opus) from a built-in HTTP server.
- SDL2 layer: renderer readback with GPU downscaling, `SDL_AudioSpec` conversion, and the Android multicast lock.
- Tested on Windows 11 x64 and Android 8.0 – 17 (arm64-v8a, armeabi-v7a), streaming to a TV with Chromecast built-in.
