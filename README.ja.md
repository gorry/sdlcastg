# sdlcastg

**SDL2 のアプリの画面と音を、Google Cast 対応機器（Chromecast 内蔵の TV、Chromecast、Nest Hub など）へ送るライブラリ**です。Windows と Android で動き、Google の SDK は使いません。

[English](README.md)

アプリが描いた絵と鳴らした音を、その場で WebM（映像 VP8 + 音声 Opus）にエンコードします。できた流れをアプリの中の HTTP サーバーで配り、受信側の **Default Media Receiver**（標準の受信アプリ）に再生させます。C のライブラリと、その上の薄い SDL2 の層からなります。MDX プレーヤー [mxv2](https://github.com/gorry/mxv2) のために作り、mxv2 ではプレーヤーの画面を TV に出すのに使っています。

非公式の送信側です。Google とは関係がなく、Google の承認も受けていません。「Google Cast」「Chromecast」は Google LLC の商標です。

## できること

- **探す**: mDNS（`_googlecast._tcp`）で機器を探し、名前・機種・アドレス・画面の有無を得ます。
- **操る**: TLS の上の Cast V2 で話します。Default Media Receiver（`CC1AD845`）を起こし、流れを読み込ませ、再生の状態を追い、音量を変えます。心拍の応答はライブラリがします。
- **その場で流す**: RGBA の絵と 48kHz ステレオの int16 の音を、実時間でエンコードします。音声を時計にし、絵にも同じ時計の時刻を付けるので、TV でも絵と音がそろいます。エンコードのスレッド、WebM の書き出し、HTTP サーバーは中にあります。
- **SDL2 の層**: `SDL_Renderer` の読み出し、どんな `SDL_AudioSpec` の音でも受ける変換、Android の Wi-Fi のマルチキャストのロックを受け持ちます。
- **ふつうのキャスト**: URL や手元のファイル（中の HTTP サーバーで配る）を再生させることもできます。

Android の OS にある画面のミラーと比べると、次の違いがあります。
- TV に出るのはアプリの画面だけで、関係のない通知は映りません。
- TCP で送り、受信側が数秒ぶん溜めるので、途切れにくくなります。

その代わり、**TV には 4 秒ほど遅れて出ます**。視聴用なら困りませんが、ゲームプレイ用には向きません。

## 状態

まだ若いライブラリで、版が変わると API も変わることがあります。確かめた組み合わせは次のとおりです。

| 送る側 | 受信側 |
|---|---|
| Windows 11 x64（Visual Studio 2022） | Chromecast 内蔵の TV（Android TV） |
| Android 8.0〜17（arm64-v8a、armeabi-v7a） | 同上 |

Linux は、Android と同じ POSIX の書き方なので動く見込みですが、試していません。macOS では SIGPIPE の対策を直す必要があります。

## 最小の使い方（SDL2）

```c
#include "sdlcastg_sdl.h"

SDLCastG_Init();

/* 1. 探す */
SDLCastG_StartDiscoverySDL();
SDL_Delay(3000);
SDLCastG_Device list[16];
int n = SDLCastG_GetDevices(list, 16);
SDLCastG_StopDiscoverySDL();

/* 2. つないで流し始める（Connect は TLS の握手まで待つ） */
SDLCastG_Connect(list[0].address, list[0].port);
/* …… SDLCastG_GetStatus() が SDLCASTG_CONNECTING でなくなるまで待つ …… */
SDLCastG_StreamConfig cfg;
SDLCastG_DefaultStreamConfig(&cfg);   /* 1280x720、30fps */
cfg.title = "My App";
SDLCastG_StartStream(&cfg);

/* 3a. 音: 音声のコールバックから、開いたときの形式のまま */
SDLCastG_SubmitAudioSDL(buffer, bytes, &obtainedSpec);

/* 3b. 絵: 毎フレーム、描く処理を挟んで */
SDLCastG_BeginRendererFrame(renderer);
/* …… いつもどおり描く …… */
SDLCastG_EndRendererFrame(renderer, NULL, -1);  /* -1 は「いま」。見せている音の時刻を渡してもよい */
SDL_RenderPresent(renderer);

/* 4. やめる */
SDLCastG_StopStream();
SDLCastG_StopApp();
SDLCastG_Disconnect();
SDLCastG_Quit();
```

- `tools/sdlcastgdemo.cpp` が、動く小さな見本です。
- mxv2 の [`src/cast.cpp`](https://github.com/gorry/mxv2/blob/main/src/cast.cpp) が実際の組み込みの例です。つなぐ処理を別スレッドで行い、送っている間は手元の音を消し、TV のリモコンの一時停止・再開に追従し、受信側が詰まったときに立て直します。
- 関数はどのスレッドから呼んでもかまいません。文字列は UTF-8 です。同時につなげるのは 1 台です。

### 知っておくこと

- **時刻**: 流れの時刻は、渡した音の長さです。絵が「いま聞こえている音」（最後に渡した音ではなく）に合わせて描かれているなら、その音の時刻を `ptsMs` に渡してください。受信側で絵と音がそろいます。今の流れの時刻は `SDLCastG_StreamTimeMs()` で分かります。
- **読み出しの重さ**: スマートフォンの画面を GPU からまるごと読み出すと、20ms 以上かかることがあります。`SDLCastG_BeginRendererFrame` / `SDLCastG_EndRendererFrame` を使うと、流している間は SDL にテクスチャへ描かせ、GPU で縮めてから読み出します。流していなければ何もしません。間で描画先を切り替えるときは、戻す先を `NULL` ではなく**元の描画先**にしてください。
- **流す量の下限**: ほとんど止まった絵は小さく縮みすぎ、受信側の読み込みが詰まって再生が止まります。そのため、WebM の `Void`（読み飛ばされる詰め物）で `minKbps`（既定 1000）まで埋めます。
- **受信側のリモコン**: ライブの流れは本当には一時停止も早送りもできません。TV で何をされたかは受信側の再生の状態（`SDLCastG_GetStatus`。`prevPlayerState` も）から推し量ります。立て直しには `SDLCastG_ReloadStream` と `SDLCastG_SeekToLive` を使います。どう扱うかの一例は mxv2 の `cast.cpp` にあります。
- **ファイアウォール**: 受信側は、アプリの中の HTTP サーバーへ流れを取りに来ます。Windows では初回にファイアウォールの許可を求められ、拒否すると TV はくるくるのまま待ち続けます。`SDLCastG_GetStreamStats()` の `clients` を見て、時間切れにするとよいでしょう。
- **安全性**: 受信側の証明書は自己署名なので、検証しません。流している間、HTTP サーバーはすべての口で待ち受けます。
- **Android**:
  - マニフェストに `INTERNET` と `CHANGE_WIFI_MULTICAST_STATE` が要ります。マルチキャストのロックを取らないと、mDNS の応答が届かない端末があります。
  - STL はライブラリと同じ `c++_static` にしてください。
  - TLS の乱数の種は `/dev/urandom` から取ります。古いカーネル（3.18）の端末では、`getrandom(2)` が何分も止まったためです。
  - 送る絵が大きいほど CPU を食います。スマートフォンなら 854x480・30fps が目安です。

## ビルド

[BUILD.ja.md](BUILD.ja.md)（[English](BUILD.md)）を見てください。要点は次のとおりです。
- **mbedTLS 3.6** は、ソースからライブラリと一緒にビルドします。
- **libvpx・Opus・libyuv** は vcpkg で作ります。Android 用の triplet と、NEON を使うように直した libvpx の移植を同梱しています。
- **SDL2** が要るのは、SDL の層と見本だけです。

自分のプロジェクトから使うときは次のようにします。

```cmake
set(SDLCASTG_MBEDTLS_DIR "${CMAKE_SOURCE_DIR}/third_party/mbedtls-3.6.7")        # 任意
set(SDLCASTG_VCPKG_INSTALLED_DIR "${CMAKE_SOURCE_DIR}/third_party/vcpkg_installed") # 任意
add_subdirectory(sdlcastg)
target_link_libraries(myapp PRIVATE sdlcastg_sdl)   # SDL を使わないなら sdlcastg
```

取り込む側に `sdl2` という名前のターゲットがあれば、SDL の層はそれを使います。

## 見本

| プログラム | すること |
|---|---|
| `castplay` | 機器を探す、1 台の状態を見る、ファイルや URL を再生させる、作った絵と音を流す（`--live`）。Android の端末のシェルでも動きます。 |
| `sdlcastgdemo` | SDL2 で描いた絵を流します。毎秒の頭にビープと白い四角を出すので、絵と音のずれを確かめられます。 |
| `sdlcastg_encodetest` | 受信側なしで WebM のファイルへ書き出し（ffprobe / ffmpeg で確かめる用）、エンコーダーの重さも測ります。 |

## 設計のメモ

[docs/design.md](docs/design.md) に、仕組みと、作る途中でつまずいたこと（遅れ、時刻の付け方、受信側のくせ、Android の落とし穴）を書いています。

## ライセンス

Apache License 2.0 です。[LICENSE](LICENSE) と [NOTICE](NOTICE) を見てください。使っている第三者のライブラリは NOTICE にあります。
