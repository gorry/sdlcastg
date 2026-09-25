/* sdlcastg - SDL2 から渡すための薄い層
 *
 * sdlcastg.h の本体は SDL に依存しない（RGBA と 48kHz ステレオの int16 を受け取る）。
 * ここは SDL2 のアプリがそのまま渡せるように、
 *   - SDL_Renderer の描画結果を読み出して渡す
 *   - SDL_AudioSpec の形式の音声を 48kHz ステレオの int16 へ変換して渡す
 *   - Android で探すときに、Wi-Fi のマルチキャストのロックを取る
 * だけを受け持つ。リンクするのは sdlcastg_sdl（と SDL2）。
 */

#ifndef SDLCASTG_SDL_H
#define SDLCASTG_SDL_H

#include <SDL.h>

#include "sdlcastg.h"

#ifdef __cplusplus
extern "C" {
#endif

/* いまの描画先（SDL_SetRenderTarget で選んだテクスチャ。無ければ窓）を読み出して
 * 渡す。SDL_RenderPresent の前に呼ぶこと（Present のあとは中身が決まっていない）。
 * fps で間引くので毎フレーム呼んでよい（頃合いでなければ読み出さずに 0 を返す）。
 * 渡したら 1、誤りなら負。 */
int SDLCastG_SubmitVideoFromRenderer(SDL_Renderer *renderer, int64_t ptsMs);

/* 上と同じだが、描画先の rect の範囲だけを読み出す（NULL なら全体）。
 * 窓の中の一部（アプリの画面が収まっている範囲）だけを送るとき用。 */
int SDLCastG_SubmitVideoFromRendererRect(SDL_Renderer *renderer, const SDL_Rect *rect,
                                         int64_t ptsMs);

/* 読み出しを軽くする描き方（任意）。1 フレームを描き始める前に Begin、表示
 * （SDL_RenderPresent）の直前に End を呼ぶ。流している間は、窓と同じ大きさの
 * テクスチャへ描かせておき、End で rect の範囲（NULL なら全体）を**送る大きさへ
 * GPU で縮めてから**読み出して渡し、そのあと窓へ写す。窓の大きさのまま読み出す
 * （SDLCastG_SubmitVideoFromRenderer）より読み出す量がずっと少ない（Xperia Ace III で
 * 720x1352 を読むのに 22ms かかり、30fps に届かなかった）。
 * 流していなければ何もしない（普段どおり窓へ描く）。
 * 描いている途中で SDL_SetRenderTarget を使うなら、戻すときは NULL ではなく
 * 元の描画先（SDL_GetRenderTarget で控えたもの）へ戻すこと。
 * End は渡したら 1、渡さなかったら 0、誤りなら負。 */
int SDLCastG_BeginRendererFrame(SDL_Renderer *renderer);
int SDLCastG_EndRendererFrame(SDL_Renderer *renderer, const SDL_Rect *rect, int64_t ptsMs);
/* End の代わりに、このフレームは渡さずに窓へ写すだけにする（Begin で描画先を
 * 替えていなければ何もしない）。 */
void SDLCastG_EndRendererFrameNoVideo(SDL_Renderer *renderer);

/* 描画の装置が作り直された（SDL_RENDER_DEVICE_RESET / SDL_RENDER_TARGETS_RESET）
 * ときに呼ぶ。上の描き方で使うテクスチャを捨て、次のフレームで作り直す。 */
void SDLCastG_ResetRendererTextures(void);

/* spec の形式の音声を渡す（形式・レート・チャンネル数は何でもよい。SDL_AudioStream で
 * 48kHz ステレオの int16 へ変換する）。音声のコールバックの中から呼んでよい。 */
int SDLCastG_SubmitAudioSDL(const void *data, int bytes, const SDL_AudioSpec *spec);

/* 探す（sdlcastg.h の SDLCastG_StartDiscovery / StopDiscovery と同じ）。Android では
 * Wi-Fi のマルチキャストのロック（WifiManager.MulticastLock）を取ってから探し、
 * 止めるときに放す。取らないと、端末によっては mDNS の応答が届かない。
 * アプリのマニフェストに android.permission.CHANGE_WIFI_MULTICAST_STATE が要る
 * （無ければロックなしで探す）。ほかの OS ではそのまま呼ぶだけ。 */
int SDLCastG_StartDiscoverySDL(void);
void SDLCastG_StopDiscoverySDL(void);

#ifdef __cplusplus
}
#endif

#endif /* SDLCASTG_SDL_H */
