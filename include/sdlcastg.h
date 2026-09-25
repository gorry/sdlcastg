/* sdlcastg - SDL2 の映像と音声を Google Cast 対応機器に送るライブラリ
 *
 * 仕組み（docs/design.md）: 受信側の標準の受信アプリ（Default Media Receiver）に
 * 「こちらの HTTP サーバーの URL」を渡して、取りに来てもらう。やりとりは
 * Cast V2（TLS の上の protobuf + JSON）。
 *
 * 2 通りの使い方がある:
 *   - URL やファイルを再生させる（SDLCastG_LoadURL / SDLCastG_ServeFile）
 *   - 映像と音声をその場でエンコードして流す（SDLCastG_StartStream のあと
 *     SDLCastG_SubmitAudio / SDLCastG_SubmitVideoRGBA を呼び続ける）。映像は VP8、
 *     音声は Opus、入れ物は WebM。受信側では 4 秒ほど遅れて出る。
 *     SDL の描画結果や SDL_AudioSpec の形式のままの音声を渡すなら
 *     sdlcastg_sdl.h を使う。
 *
 * 同時につなげる受信側は 1 台。関数はどのスレッドから呼んでもよい
 * （中で 1 つの鍵を取る）。文字列はすべて UTF-8。
 */

#ifndef SDLCASTG_H
#define SDLCASTG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDLCastG_Device {
	char id[64];       /* 固有の ID（mDNS の TXT の id=） */
	char name[128];    /* 名前（fn=） */
	char model[64];    /* 機種（md=） */
	char address[48];  /* IPv4 */
	int port;          /* ふつう 8009 */
	/* 能力のビット（TXT の ca=）。-1 なら分からない。SDLCASTG_CAP_VIDEO_OUT が
	 * 無いもの（画面の無いスピーカー）には映像を送っても出ない。 */
	int capabilities;
} SDLCastG_Device;

#define SDLCASTG_CAP_VIDEO_OUT 1

typedef enum SDLCastG_State {
	SDLCASTG_DISCONNECTED = 0,
	SDLCASTG_CONNECTING,
	SDLCASTG_CONNECTED,   /* つながった。受信アプリはまだ */
	SDLCASTG_LAUNCHING,   /* 受信アプリを起こしている */
	SDLCASTG_LOADING,     /* 再生を頼んだ */
	SDLCASTG_BUFFERING,
	SDLCASTG_PLAYING,
	SDLCASTG_PAUSED,
	SDLCASTG_IDLE,        /* 再生が終わった・止められた（idleReason を見る） */
	SDLCASTG_ERROR
} SDLCastG_State;

typedef struct SDLCastG_Status {
	SDLCastG_State state;
	char error[160];           /* SDLCASTG_ERROR / 切れたときの理由 */
	char runningApp[64];       /* 受信側でいま動いているアプリの名前 */
	float volume;              /* 受信側の音量 0..1 */
	int muted;
	char playerState[16];      /* IDLE / BUFFERING / PLAYING / PAUSED */
	/* その前の状態。IDLE になったのが何からかを見るため（TV のリモコンの停止は
	 * PLAYING / PAUSED から、飛ばしそこないは BUFFERING から CANCELLED になる）。 */
	char prevPlayerState[16];
	char idleReason[16];       /* FINISHED / ERROR / CANCELLED / INTERRUPTED / STOPPED / CLOSED */
	double currentTime;        /* 受信側の再生位置（秒） */
} SDLCastG_Status;

/* 0 なら成功、負なら失敗（理由は SDLCastG_GetError）。 */
int SDLCastG_Init(void);
void SDLCastG_Quit(void);
const char *SDLCastG_GetError(void);
/* ライブラリの版（"2026.0925.1" の形。sdlcastg_version.h の SDLCASTG_VERSION と同じ）。 */
const char *SDLCastG_GetVersion(void);

/* 探す。見つかったものは SDLCastG_GetDevices で読む（裏で更新され続ける）。 */
int SDLCastG_StartDiscovery(void);
void SDLCastG_StopDiscovery(void);
/* 見つかった数を返し、out へ max 台まで写す。 */
int SDLCastG_GetDevices(SDLCastG_Device *out, int max);
/* IP が分かっている 1 台に直に尋ねる（名前と機種を知るため）。 */
int SDLCastG_ProbeDevice(const char *address, SDLCastG_Device *out);

/* つなぐ。受信側の表示は変わらない（受信アプリはまだ起こさない）。 */
int SDLCastG_Connect(const char *address, int port);
/* 別れの挨拶をして切る。受信アプリは止めない（止めるなら先に SDLCastG_StopApp）。 */
void SDLCastG_Disconnect(void);

/* 受信アプリを起こして url を再生させる。contentType は "video/webm" など。
 * live が 0 以外なら流しっぱなしのストリームとして頼む。 */
int SDLCastG_LoadURL(const char *url, const char *contentType, int live, const char *title);

/* ファイルを手元の HTTP サーバーで配り、受信側から取りに来られる URL を
 * urlOut に返す（つないでからでないと、どの口の住所を使うか決まらない）。
 * contentType が NULL なら拡張子から決める。 */
int SDLCastG_ServeFile(const char *path, const char *contentType, char *urlOut, size_t urlSize);

/* 受信アプリを止める（受信側はアプリの無い状態に戻る）。 */
int SDLCastG_StopApp(void);
/* 受信側の音量 0..1。 */
int SDLCastG_SetVolume(float level);

/* 受信側の再生を、流れの今の位置から遅れ（受信側がふだん溜めている長さ）だけ手前へ
 * 飛ばす。受信側で飛ばそうとして（TV のリモコンの左右キー）、まだ届いていない、
 * あるいはもう無いところを待ち続けているときに、届いているところへ飛ばし直して
 * 飛ばしを終わらせる。behindMs は流れの今の位置からどれだけ手前か。 */
int SDLCastG_SeekToLive(int behindMs);

/* ---- 映像と音声を流す --------------------------------------------------- */

typedef struct SDLCastG_StreamConfig {
	int width;        /* 送る映像の大きさ（既定 1280x720。偶数に丸める） */
	int height;
	int fps;          /* 既定 30 */
	int videoKbps;    /* 既定 2000 */
	int audioKbps;    /* 既定 128 */
	/* 流す量の下限（既定 1000）。動きの少ない絵は小さくなりすぎ、受信側が
	 * 一定のバイト数を溜めてから読むために再生が詰まる。足りないぶんは
	 * 読み飛ばされる詰め物（WebM の Void）で埋める。負なら詰めない。 */
	int minKbps;
	const char *title;  /* 受信側に出る題名（NULL 可） */
} SDLCastG_StreamConfig;

typedef struct SDLCastG_StreamStats {
	int64_t audioMs;          /* 流れに入れた音声の長さ（＝流れの時刻） */
	int64_t videoMs;          /* 最後に入れた映像の時刻 */
	int64_t silenceMs;        /* 音声が来なかったので足した無音の長さ */
	uint64_t videoFrames;
	uint64_t droppedFrames;
	uint64_t sameTimeFrames;  /* 前の絵と同じ時刻（fps の枠）だったので捨てた映像の数 */
	double encodeMsAvg;       /* 映像 1 枚のエンコードにかかる時間（変換込み） */
	int clients;              /* 取りに来ている受信側の数 */
	int64_t bytesSent;
	/* 受信側の時刻 0 が流れの上のどこか (ms)。受信側には、つないだところから
	 * 0 で始まる時刻で渡している。受信側の再生位置（SDLCastG_Status の
	 * currentTime）にこれを足すと流れの時刻になり、audioMs との差が遅れになる。
	 * 複数つないでいるときは、一番古いもの（ふつうは受信側）について。 */
	int64_t clientBaseMs;
} SDLCastG_StreamStats;

void SDLCastG_DefaultStreamConfig(SDLCastG_StreamConfig *cfg);

/* エンコードを始め、受信アプリに流れを再生させる（つないでからでないと使えない）。 */
int SDLCastG_StartStream(const SDLCastG_StreamConfig *cfg);
/* エンコードをやめる。受信アプリは止めない（止めるなら SDLCastG_StopApp）。 */
void SDLCastG_StopStream(void);

/* 受信アプリに流れを読み込み直させる（溜まっているぶんを捨てて、今の位置から
 * 再生し直す）。受信側で一時停止されたあと、再開するときに使う（止めていた
 * 間も流れは進むので、そのまま再開すると止めていた長さだけ遅れる）。
 * 受信側は数秒読み込み中になる。 */
int SDLCastG_ReloadStream(void);

/* 音声: 48kHz ステレオの int16（左右交互）。渡した長さがそのまま流れの時刻になる
 * （音声が時計）。途切れたら無音で埋めるので、止まっている間は渡さなくてよい。 */
int SDLCastG_SubmitAudio(const int16_t *pcm, int frames);

/* 映像: RGBA（メモリ上で R, G, B, A の順）。大きさは何でもよく、縦横比を保って
 * 送る大きさへ収める。ptsMs はこの絵の流れの上の時刻（SDLCastG_StreamTimeMs と
 * 同じ時計）。負なら「いま」。来なくなったら最後の絵を出し直す。 */
int SDLCastG_SubmitVideoRGBA(const void *pixels, int width, int height, int pitch, int64_t ptsMs);

/* 次の映像を受け取る頃合いか（fps で間引く）。偽なら画面を読み出さなくてよい。 */
int SDLCastG_WantVideoFrame(void);

/* これまでに渡した音声の長さ (ms)。 */
int64_t SDLCastG_StreamTimeMs(void);

/* 流しているなら 1 を返し、送る映像の大きさを w, h に入れる（NULL 可）。 */
int SDLCastG_GetStreamSize(int *w, int *h);

void SDLCastG_GetStreamStats(SDLCastG_StreamStats *out);

void SDLCastG_GetStatus(SDLCastG_Status *out);
const char *SDLCastG_StateName(SDLCastG_State s);

#ifdef __cplusplus
}
#endif

#endif /* SDLCASTG_H */
