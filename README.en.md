# sdlcastg

**Send the screen and sound of an SDL2 application to a Google Cast device** (a TV with Chromecast built-in, a Chromecast dongle, a Nest Hub, …) — from Windows and Android, with no Google SDK.

[日本語](README.md)

sdlcastg encodes what your app draws and plays into a live WebM stream (VP8 video + Opus audio). It serves the stream over HTTP from inside the app and asks the device's **Default Media Receiver** to play it. It is a small C library with a thin SDL2 layer on top. It was written for [mxv2](https://github.com/gorry/mxv2), an MDX music player, and is used there to show the player on a TV.

sdlcastg is an unofficial sender. It is not affiliated with or endorsed by Google. "Google Cast" and "Chromecast" are trademarks of Google LLC.

## What it does

- **Discovery**: finds devices with mDNS (`_googlecast._tcp`). It gets each device's name, model, address, and whether it has a screen.
- **Control**: talks Cast V2 over TLS. It launches the Default Media Receiver (`CC1AD845`), loads the stream, follows the player state, and sets the volume. The heartbeat is handled for you.
- **Live streaming**: encodes RGBA frames and 48 kHz stereo int16 PCM in real time. Audio is the clock, and video frames carry timestamps on the same clock, so picture and sound stay in sync on the TV. The encoder threads, the WebM muxer and the HTTP server are all inside.
- **SDL2 layer**: reads back an `SDL_Renderer`, converts any `SDL_AudioSpec`, and takes the Wi-Fi multicast lock on Android.
- **Plain casting**: plays a URL or a local file (served by the built-in HTTP server) on the device.

Compared with the screen mirroring built into Android:
- Only your app's picture reaches the TV. Unrelated notifications do not.
- The stream goes over TCP with a few seconds of buffering on the receiver, so it rarely stutters.

The price is **latency: the TV shows everything about 4 seconds late**. That is fine for a player, but not for a game.

## Status

Young. The API may still change between versions. Tested with:

| Sender | Receiver |
|---|---|
| Windows 11 x64 (Visual Studio 2022) | TV with Chromecast built-in (Android TV) |
| Android 8.0 – 17 (arm64-v8a, armeabi-v7a) | same |

Linux may work (the POSIX code paths are shared with Android) but has not been tried. macOS needs a change for `SIGPIPE`.

## Minimal use (SDL2)

```c
#include "sdlcastg_sdl.h"

SDLCastG_Init();

/* 1. find devices */
SDLCastG_StartDiscoverySDL();
SDL_Delay(3000);
SDLCastG_Device list[16];
int n = SDLCastG_GetDevices(list, 16);
SDLCastG_StopDiscoverySDL();

/* 2. connect and start streaming (connect blocks until the TLS handshake is done) */
SDLCastG_Connect(list[0].address, list[0].port);
/* ... wait until SDLCastG_GetStatus() reports something other than SDLCASTG_CONNECTING ... */
SDLCastG_StreamConfig cfg;
SDLCastG_DefaultStreamConfig(&cfg);   /* 1280x720, 30 fps */
cfg.title = "My App";
SDLCastG_StartStream(&cfg);

/* 3a. audio: from your audio callback, in whatever format you opened */
SDLCastG_SubmitAudioSDL(buffer, bytes, &obtainedSpec);

/* 3b. video: every frame, around your drawing */
SDLCastG_BeginRendererFrame(renderer);
/* ... draw as usual ... */
SDLCastG_EndRendererFrame(renderer, NULL, -1);  /* -1 = "now"; or the time of the sound you are showing */
SDL_RenderPresent(renderer);

/* 4. stop */
SDLCastG_StopStream();
SDLCastG_StopApp();
SDLCastG_Disconnect();
SDLCastG_Quit();
```

- `tools/sdlcastgdemo.cpp` is a complete, small program.
- mxv2's [`src/cast.cpp`](https://github.com/gorry/mxv2/blob/main/src/cast.cpp) is a real integration. It runs connecting on a worker thread, mutes local sound while casting, follows the TV remote's pause and play, and recovers when the receiver gets stuck.
- All functions are thread-safe. Strings are UTF-8. One device at a time.

### Things to know

- **Timing.** The stream time is the amount of audio you have submitted. If your picture follows the sound that is *currently audible* (not the sound you last queued), pass that time as `ptsMs`. The receiver then keeps them together. `SDLCastG_StreamTimeMs()` tells you the current stream time.
- **Readback cost.** Reading a full phone screen back from the GPU can take 20 ms or more. `SDLCastG_BeginRendererFrame` / `SDLCastG_EndRendererFrame` make SDL draw into a texture while streaming, then downscale it on the GPU before reading. When not streaming they do nothing. If you switch render targets in between, restore the *previous* target, not `NULL`.
- **Minimum bit rate.** A mostly still picture compresses so well that the receiver starves and stutters. The stream is padded to `minKbps` (default 1000) with WebM `Void` elements.
- **The receiver's remote.** A live stream cannot really be paused or sought. The receiver's player state (`SDLCastG_GetStatus`, including `prevPlayerState`) shows what the user did on the TV. `SDLCastG_ReloadStream` and `SDLCastG_SeekToLive` help you recover. See mxv2's `cast.cpp` for one policy.
- **Firewall.** The receiver fetches the stream from an HTTP server inside your app. On Windows, the first run asks for firewall permission. If it is denied, the TV waits forever (the spinner never stops). Consider a timeout that checks `SDLCastG_GetStreamStats().clients`.
- **Security.** The receiver's certificate is self-signed and is not verified. The HTTP server listens on all interfaces while streaming.
- **Android.**
  - Add `INTERNET` and `CHANGE_WIFI_MULTICAST_STATE` to your manifest. Without the multicast lock, some phones never see the mDNS answers.
  - Link with the same `c++_static` STL.
  - The library takes its TLS entropy from `/dev/urandom`: `getrandom(2)` blocked for minutes on an old 3.18 kernel.
  - Bigger frames cost more CPU. 854x480 at 30 fps is a good default for phones.

## Building

See [BUILD.en.md](BUILD.en.md) ([日本語](BUILD.md)). In short:
- **mbedTLS 3.6** is built from source together with the library.
- **libvpx, Opus and libyuv** are built with vcpkg. Triplets for Android and a NEON-enabled libvpx port are included.
- **SDL2** is needed only for the SDL layer and the demo.

To use it from your project:

```cmake
set(SDLCASTG_MBEDTLS_DIR "${CMAKE_SOURCE_DIR}/third_party/mbedtls-3.6.7")        # optional
set(SDLCASTG_VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/third_party/vcpkg_installed") # optional
add_subdirectory(sdlcastg)
target_link_libraries(myapp PRIVATE sdlcastg_sdl)   # or sdlcastg without SDL
```

If your project already has a target named `sdl2`, the SDL layer uses it.

## Sample programs

| Program | What it does |
|---|---|
| `castplay` | List devices, show a device's status, play a file or URL, or stream a generated test picture and tone (`--live`). Runs in an Android shell too. |
| `sdlcastgdemo` | Draws with SDL2 and streams it, with a beep and a white square every second to check audio/video sync. |
| `sdlcastg_encodetest` | Encodes to a WebM file without a receiver (for ffprobe / ffmpeg), and benchmarks the encoder. |

## Design notes

[docs/design.md](docs/design.md) (in Japanese) explains how it works and what went wrong on the way: latency, timestamps, receiver quirks, and Android pitfalls.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE). The third-party libraries are listed in NOTICE.
