# sdlcastg の設計と、作る途中で分かったこと

sdlcastg の仕組みと、決めたことの理由、つまずいたところの記録です。数値は、送る側が
Windows 11 のパソコン・Android の端末、受信側が Chromecast 内蔵の TV（Android TV の
Cast 受信部）で測ったものです。

## 1. 仕組み

Google Cast 対応機器へ映像を送る方法は、大きく 2 通りあります。

| 方法 | 受信側 | 遅れ | 手間 |
|---|---|---|---|
| **A. 標準の受信アプリに URL を渡す** | Default Media Receiver（アプリ ID `CC1AD845`。登録不要） | 数秒（受信側が溜める） | 中 |
| B. 自前の受信アプリ + WebRTC | Google Cast の開発者登録と、HTTPS で置く HTML の受信アプリ | 0.2 秒ほど | 大 |

sdlcastg は **A** です。絵と音は同じ流れに入るので、互いにはずれません。遅れるのは、
手元の操作が TV に届くまでだけです。

流れは次のとおりです。

1. **探す**: mDNS で `_googlecast._tcp.local` を問い合わせ、名前（TXT の `fn=`）、機種（`md=`）、
   能力（`ca=`）、アドレス、ポート（ふつう 8009）を得ます。
2. **つなぐ**: 8009 番へ TLS でつなぎます（証明書は自己署名なので検証しません）。やりとりは
   Cast V2 です。4 バイトの長さに protobuf の `CastMessage` が続き、中身は名前空間ごとの JSON です。
   - `urn:x-cast:com.google.cast.tp.connection` … CONNECT / CLOSE
   - `urn:x-cast:com.google.cast.tp.heartbeat` … 5 秒ごとの PING / PONG
   - `urn:x-cast:com.google.cast.receiver` … LAUNCH、STOP、状態、音量
   - `urn:x-cast:com.google.cast.media` … LOAD（URL・型・`streamType: LIVE`）、状態、SEEK
3. **流す**: 送る側が小さな HTTP サーバーを立て、エンコードした WebM を返し続けます。受信側は
   その URL を取りに来ます。

公式の SDK は Android / iOS / Chrome 用だけで、デスクトップの C/C++ 用はありません。
プロトコルは VLC、pychromecast、go-chromecast なども実装していて、送る側に認証は要りません。

## 2. 部品

| ファイル | 中身 |
|---|---|
| `include/sdlcastg.h` | 公開 API（C）。SDL に依存しない |
| `include/sdlcastg_sdl.h`、`src/sdlcastg_sdl.cpp` | SDL2 の層。描画結果の読み出し、音の形式の変換、Android のマルチキャストのロック |
| `src/net.*` | ソケットの包み（Winsock / POSIX）、口の一覧、手元の住所 |
| `src/json.*` | 木に読むだけの小さな JSON |
| `src/castmsg.*` | `CastMessage` の手書きの符号化・復号（使う項目は 7 つだけなので、protobuf のライブラリは使わない） |
| `src/channel.*` | TLS（mbedTLS）と心拍。**入出力は 1 本のスレッド**（mbedTLS は読みと書きを別スレッドから同時にできない）。送信は待ち行列に積み、読み取りを 50ms で区切って、その合間に書く。受信側の PING には PONG を返し、15 秒何も来なければ切る |
| `src/session.*` | CONNECT / GET_STATUS / LAUNCH / LOAD / STOP / SET_VOLUME / SEEK。RECEIVER_STATUS と MEDIA_STATUS から状態を作る |
| `src/discovery.*` | mDNS。1 台に直に尋ねる Probe も |
| `src/httpserver.*` | GET / HEAD。ファイルは Range（206）に応え、ライブは 200 で流しっぱなし |
| `src/webm.*` | ライブ向けの WebM の書き出し（自前）。Segment の大きさは不明のまま、Cues なし、Cluster は 500ms かキーフレームで区切る |
| `src/livesource.*` | HTTP で配る中身。受信側ごとに「つないだあとのキーフレーム」から、時刻を 0 に振り直して渡す |
| `src/encoder.*` | VP8（libvpx の実時間モード・CBR・2 秒ごとのキーフレーム）と Opus（48kHz ステレオ・20ms）。縮小と色の変換は libyuv |

スレッドは、エンコード、HTTP 配信、Cast の制御の 3 系統です。呼ぶ側は待たされません
（Submit は積むだけで、映像が溢れたら古いものから捨てます。音は捨てません）。

## 3. 形式を WebM（VP8 + Opus）にした理由

ffmpeg で作った流れを Default Media Receiver に 60 秒ずつ再生させて比べました。

| | WebM（VP8 + Opus） | fMP4（H.264 + AAC） |
|---|---|---|
| LAUNCH から受信アプリが起きるまで | 6.4 秒 | 6.1 秒 |
| 取りに来てから PLAYING まで | 約 4 秒 | 約 4.4 秒 |
| 遅れ | **3.84 秒で一定** | 4.36〜4.84 秒で揺れる |
| TV での見え方 | 60 秒間良好 | 途中で引っかかり、その後中断 |

- VP8・Opus・WebM は、Default Media Receiver の標準の対応形式で、どの世代の機器でも再生できます。libvpx・libopus は BSD の
  ライセンスで、Windows でも NDK でもビルドできます。
- 受信側は `Range: bytes=0-` 付きの GET を 1 回だけ送り、あとは読み続けます。**Range には応えず、
  200 で流しっぱなし**（Content-Length なし、`Connection: close`）で問題ありませんでした。
- OS の画面ミラー（Cast Streaming）は、受信側の溜めを浅くして遅れを縮める作りなので、揺れに
  弱いという特徴があります。sdlcastg は TCP で 4 秒ほど溜めるので、遅れる代わりに途切れにくくなっています。

## 4. 遅れと、受信側の読み方

- **流す量が少ないと詰まる。** 動きの少ない絵は VP8 で約 0.2Mbps まで縮みます。受信側は
  一定のバイト数ずつ読む作りのようで、1 回読むのに 1 秒以上かかり、そのたびに再生が詰まり
  ました。そこで Cluster の末尾に WebM の **Void**（読み飛ばされる詰め物）を足し、最低 1Mbps
  流すようにしています（`SDLCastG_StreamConfig.minKbps`、既定 1000）。2Mbps・4Mbps に上げても
  遅れは変わりません（受信側がもともと約 4 秒溜めるため）。
- **時刻は 0 から始める。** 受信側は 0 から再生を始めて、最初の Cluster の時刻へ飛びます。
  受信側ごとに Cluster の Timecode を振り直しています。
- **受信側の再生位置（currentTime）は、問い合わせたときにしか更新されない。** そのまま
  遅れを計算すると、「受け取った値の古さ」ぶんの 0〜2 秒ののこぎり形になりました。再生中は、
  受け取ってからの経過を足して使います。直したあとの遅れは 3.7 秒前後で平らです。
- 30 分流しても止まらず、溜め直しは始めの 1 回だけでした。

## 5. 時刻の付け方

**音声が時計**です。流れの時刻は、渡された音の長さそのものです。音が途切れたら無音で
埋めます。映像が来なくなったら、最後の絵を 1 秒ごとに出し直します。

- **映像の時刻は fps の格子に載せる。** 呼ぶ側が絵を渡した時刻をそのまま使うと、間隔が
  20〜47ms とばらつきました。受信側では、映像が音から少しずつ遅れては 10〜15 秒ごとに
  一度に合わせ直す、という動きになります（フレームが飛んだように見える）。そこで、枠 n の
  時刻を n × 1000 / fps とし、流れの時計が次の枠に入ったときに受け取るようにしています。
- **同じ枠の絵は捨てる。** 枠が埋まっていたら次の枠へ送る作りにしていたところ、後ろの絵が
  押し出され続け、映像がどんどん遅れていきました。絵の時刻が音声のコールバックごとにしか
  進まないアプリでは、30fps で読むと同じ時刻の絵が続くためです。今は捨てて、数を
  `sameTimeFrames` に数えます（キーフレームを頼まれているときと、出し直しだけは次の枠へ）。
- **音と映像は、時刻順に混ぜて書く。** 以前の書き出しは「どのトラックでも、前の塊より時刻を
  戻さない」と揃えていました。アプリの絵が「いま聞こえている音」の時刻（＝渡し終えた音より
  装置のバッファぶん古い）だと、書くときにはその時刻の音がもう書いてあります。そのため
  **絵の時刻が音の時刻まで押し出され**、Android で 43ms、パソコンで 10ms ほど映像が遅れて
  いました。今は音を 300ms 溜め、音と映像を時刻順に混ぜて書きます（`WriteOrdered`）。
  受信側の遅れはそのぶん延びます（約 4.0 → 4.3 秒）。
- 受け取る頃合いの見積もりには、最後に音声が来てからの経過も足します。Android のように
  音声が 43ms ずつまとめて来ると、渡された音の長さだけで判断すると 20fps ほどしか読めないためです。

`sdlcastg_encodetest --video-lag 100`（絵を 100ms 古い時刻で渡す）で、書き出した流れの中で
点滅がビープのすぐ後に来ること（押し出されていれば 100ms 後）を確かめられます。

## 6. 画面の読み出し

窓の大きさのまま GPU から読み出すと、縦長の画面では重くなります（Xperia Ace III で
720x1352 を 1 回 22ms。30fps に届きませんでした）。`SDLCastG_BeginRendererFrame` /
`SDLCastG_EndRendererFrame` では、流している間だけ窓と同じ大きさのテクスチャへ描かせ、
GPU で送る大きさへ縮めてから読み出します（2 倍より大きく縮めるときは、2 倍の大きさを
経由して 2x2 の平均にし、細い線が消えにくいようにしています）。読み出しは 9.6ms に
なりました。縮小と色の変換は libyuv（`ARGBScale` の `kFilterBox`、`ABGRToI420`）です。

## 7. CPU の重さ

**スマートフォンでは、時間の割合ではなくサイクル数で比べます。** 周期的な仕事に合わせて
クロックが下がるので、「1 コアの何 %」は何を変えても 60〜70% に張り付きます。
`simpleperf stat -e cpu-cycles:u` で測りました。big.LITTLE の端末では、`taskset` で
コアの種類もそろえます。

Pixel 7a、`sdlcastg_encodetest --bench --static`（一部だけが動く絵）、VP8 は 1 スレッド:

| 送る大きさ | fps | サイクル/秒 |
|---|---|---|
| 1280x720 | 30 | 0.63〜0.69G |
| 1280x720 | 20 | 0.41G |
| 854x480 | 30 | 0.41G |
| 854x480 | 20 | 0.28G |
| 640x360 | 30 | 0.27G |

- 画素数 × fps にほぼ比例します。小さいコア（A55 1.8GHz）だけだと 720p30 は間に合わず、
  映像を捨てます（音は途切れません）。**スマートフォンの既定は 854x480・30fps** を勧めます。
- **armv7 は arm64 より 3〜4 割重い**（同じ NEON でも）。
- **VP8 は 1 スレッド**にしています。複数スレッドだと行のそろい待ちを回りながら待つため、
  2 本でサイクルが 1.46 倍、4 本で 2 倍以上になり、1 枚の時間も縮みませんでした。
- `VP8E_SET_CPUUSED` 16（8 より 1 割減るだけで画質が落ちる）、`VP8E_SET_STATIC_THRESHOLD`、
  `VP8E_SET_SCREEN_CONTENT_MODE`、ビットレートの上げ下げでは軽くなりませんでした。
- 32bit 専用の古い端末（Snapdragon 617 相当）でも、送れる絵は 11〜15fps に落ちますが動きます。

## 8. 探索

- **QU ビット**（ユニキャストで返してほしい印）を立てて問い合わせます。立てないと、機器は
  224.0.0.251:5353 へ返し、こちらはそこで待っていないので受け取れませんでした。念のため
  5353 番でも待ちます。問い合わせは口（ネットワークインターフェース）ごとに送ります。
- 画面の無い機器（スマートスピーカー）も見つかります。TXT の `ca=` の bit 0（映像出力）で
  見分けられます（`SDLCastG_Device.capabilities`、`SDLCASTG_CAP_VIDEO_OUT`）。
- 自分の IP は「受信側へつないだソケットの手元側のアドレス」を使います。複数の口があっても、
  受信側から届く住所が選べます。
- Android では `WifiManager.MulticastLock` を取らないと mDNS の応答を受け取れない端末があります。
  `SDLCastG_StartDiscoverySDL` が JNI で取ります（Java のクラスは足していません。作業スレッドからだと
  アプリのクラスローダーが見えないので、FindClass を使わず手元のオブジェクトからクラスを取ります）。

## 9. 受信側のリモコン

受信側のリモコンのキーは送る側に届きません。受信側の再生の状態の移り変わりから推し量ります
（`SDLCastG_Status.playerState` と `prevPlayerState`、`idleReason`）。Android TV の Cast
受信部の例です（`supportedMediaCommands` は PAUSE、SEEK、STREAM_VOLUME、STREAM_MUTE、
EDIT_TRACKS、PLAYBACK_RATE、STREAM_TRANSFER。「次へ / 前へ」は無い）。

| 操作 | 移り変わり |
|---|---|
| 一時停止 | PLAYING → PAUSED |
| 再開 | PAUSED → BUFFERING → PLAYING |
| 停止ボタン | PLAYING → IDLE（CANCELLED）に直に |
| 左右キー（飛ばす） | PLAYING → BUFFERING →（PLAYING に戻るか、IDLE の CANCELLED / ERROR） |

- ライブの流れは本当には止められません。一時停止のあと再開すると、止めていた長さだけ
  遅れたままになります。`SDLCastG_ReloadStream`（新しい URL で LOAD し直す）で今の位置から
  再生し直させます。
- 飛ばされると、受信側は目次の無い流れの中で「まだ届いていない、あるいはもう無いところ」を
  待ち続けることがあります（くるくるが出たまま戻らない）。読み込み直すと受信側の飛ばしが
  終わらないまま「再生中」の札が残ったので、**SEEK で受信側に届いている位置（流れの今の位置
  から 4 秒手前）へ飛ばし直して、飛ばしを終わらせます**（`SDLCastG_SeekToLive`）。
- 読み込み直した直後には、前の再生の知らせ（`IDLE / INTERRUPTED` や、受信側の自前の読み込みを
  遮った `LOAD_FAILED`）が来ます。**LOAD への返事（同じ requestId の MEDIA_STATUS）で自分の
  mediaSessionId を知り、それより古い id の知らせは捨てます**。失敗の知らせも、こちらの LOAD の
  requestId のものだけを見ます。
- どう扱うかの一例（一時停止を手元にも伝える、停止ボタンで終える、溜め直しが長引いたら
  SEEK → 読み込み直し）は、mxv2 の `src/cast.cpp` にあります。

## 10. つまずいたこと

- **SIGPIPE でプロセスごと落ちる（POSIX）。** 相手が切ったソケットに書くと SIGPIPE が出ます。
  受信側が再生をやめたり、ネットワークが切れたりしただけでアプリが落ちます。HTTP の送信は
  `MSG_NOSIGNAL`、TLS の送信は mbedTLS の `mbedtls_net_send`（`write()`）を自前の送信関数に
  差し替えています。macOS には `MSG_NOSIGNAL` が無いので、`SO_NOSIGPIPE` が要ります（未対応）。
- **古いカーネルの Android で、つなぐまでに 2 分かかる。** MT6737M / Linux 3.18 / Android 8.1 の
  端末で、`psa_crypto_init` が 39 秒、`mbedtls_ctr_drbg_seed` が 86 秒止まっていました（CPU は
  ほぼ使っていない）。mbedTLS が乱数の種に使う `getrandom(2)` が待っていたためで、同じ端末でも
  `/dev/urandom` は一瞬で読めます。Android では `MBEDTLS_USER_CONFIG_FILE` で
  `MBEDTLS_NO_PLATFORM_ENTROPY` と `MBEDTLS_ENTROPY_HARDWARE_ALT` を指定し、`/dev/urandom` を読む
  `mbedtls_hardware_poll` を種にしています（125 秒 → 0.23 秒）。構造体の形が変わるので、この
  定義は mbedcrypto の PUBLIC に付け、使う側にも効かせます。2 秒以上かかったときは、ログに
  内訳（`slow connect … (psa, tcp, seed, handshake)`）を出します。
- **vcpkg の標準の libvpx は、Android を SIMD なしで作る**（`generic-gnu`）。simpleperf で
  `*_c` の関数ばかり出たことで気づきました。`ports/libvpx/` では次のように直しています。
  - ターゲットを `arm64-android-gcc` / `armv7-android-gcc` にする。
  - 環境変数 `CFLAGS` / `CXXFLAGS` / `ASFLAGS` にも `--target=...` を入れる。移植が渡す
    `--extra-cflags` は NEON の有無を調べたあとで足されるので、それだけでは NEON が
    黙って外れます（`vpx_config.h` の `HAVE_NEON 0` で分かる）。
  - `AS` の `.exe` を取る。armv7 は `--enable-thumb` を外す（GNU as 用の `-mimplicit-it` が
    付き、clang が受け付けない）。
- **vcpkg のマニフェストモードは、入れ先に triplet を 1 つしか置けない。** Windows 向けと
  Android 向けを同じ場所に置くため、クラシックモードにしています。
- **vcpkg の標準の Android の triplet で NDK が見つからない**（vcpkg 2025-06-20）。自前の
  triplet でツールチェーンを明示し、vcpkg の場所は読み込み元の `CMAKE_PARENT_LIST_FILE` から
  求めています（`VCPKG_ROOT` などの環境変数は、triplet の評価の中からは見えませんでした）。
- **クラスの中で値を与えた `static const` を参照で渡すと、最適化しないビルドでリンクに
  失敗する**（C++11。`std::chrono::milliseconds(kX)` など）。`.cpp` に定義を置いています。
- **Windows のファイアウォールで待ち受けが止められると、受信側はくるくるのまま何も言って
  こない。** 呼ぶ側で `SDLCastG_GetStreamStats().clients` を見て、時間切れにしてください
  （mxv2 は 20 秒で打ち切り、ファイアウォールを確かめるよう案内します）。
- **（アプリ側、参考）** SDL2 の Android は、窓の大きさの変化を Java の UI スレッドから配ります。
  そのとき、レンダラーのイベントの見張りも UI スレッドで描画先を切り替え、GL の控えを壊すことが
  あります。素早く回転すると画面が上下逆・赤青反転のまま固まりました。描画先のテクスチャを使う
  アプリで起きます（sdlcastg の Begin/End もテクスチャへ描かせます）。mxv2 は `SDL_SetEventFilter` で UI スレッドから
  来た大きさの知らせを捨て、描画スレッドで配り直して避けています（mxv2 の `src/main.cpp`）。
