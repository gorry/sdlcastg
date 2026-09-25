// sdlcastg - SDL2 から渡すための薄い層（include/sdlcastg_sdl.h）

#include "sdlcastg_sdl.h"

#include <mutex>
#include <vector>

#ifdef __ANDROID__
#include <jni.h>
#endif

namespace {

// 音声の変換。形式が変わったら作り直す。コールバックのスレッドから呼ばれる。
struct AudioConv {
	std::mutex mutex;
	SDL_AudioStream *stream;
	SDL_AudioFormat format;
	Uint8 channels;
	int freq;
	std::vector<int16_t> out;

	AudioConv() : stream(0), format(0), channels(0), freq(0) {}
	~AudioConv() {
		if (stream != 0) SDL_FreeAudioStream(stream);
	}
};

AudioConv &Conv() {
	static AudioConv c;
	return c;
}

std::vector<uint8_t> &PixelBuffer() {
	static std::vector<uint8_t> buf;
	return buf;
}

// SDLCastG_BeginRendererFrame / End で使うテクスチャ（メインスレッドだけ）。
struct FrameTextures {
	SDL_Renderer *renderer;
	SDL_Texture *frame;  // 窓と同じ大きさ。1 フレームぶんをここへ描かせる
	SDL_Texture *mid;    // 縮める途中（2 倍の大きさ）
	SDL_Texture *small;  // 送る大きさへ収めた大きさ。ここを読み出す
	int frameW, frameH, midW, midH, smallW, smallH;
	bool active;         // Begin で描画先を frame にした
	FrameTextures()
	    : renderer(0), frame(0), mid(0), small(0), frameW(0), frameH(0), midW(0), midH(0),
	      smallW(0), smallH(0), active(false) {}
};

FrameTextures &Tex() {
	static FrameTextures t;
	return t;
}

void DestroyTex(SDL_Texture **t) {
	if (*t != 0) SDL_DestroyTexture(*t);
	*t = 0;
}

// 描画先にできるテクスチャを、大きさが違えば作り直す。縮めるときは双線形、
// 写すときは混ぜない（アルファで下が透けないように）。
bool EnsureTarget(SDL_Renderer *r, SDL_Texture **t, int *tw, int *th, int w, int h) {
	if (*t != 0 && *tw == w && *th == h) return true;
	DestroyTex(t);
	*t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
	if (*t == 0) return false;
	SDL_SetTextureBlendMode(*t, SDL_BLENDMODE_NONE);
	SDL_SetTextureScaleMode(*t, SDL_ScaleModeLinear);
	*tw = w;
	*th = h;
	return true;
}

#ifdef __ANDROID__
// Wi-Fi のマルチキャストのロック。Java のクラスを足さずに JNI で直に呼ぶ:
//   lock = context.getApplicationContext().getSystemService("wifi")
//                 .createMulticastLock("sdlcastg");
//   lock.setReferenceCounted(false); lock.acquire();
// FindClass は使わない（作業スレッドからだとアプリのクラスローダーが見えない
// ことがある）。クラスはすべて手元のオブジェクトから取る。
std::mutex g_lockMutex;
jobject g_multicastLock = 0;  // グローバル参照

bool ClearException(JNIEnv *env) {
	if (!env->ExceptionCheck()) return false;
	env->ExceptionClear();
	return true;
}

jobject CallObject(JNIEnv *env, jobject obj, const char *name, const char *sig, jobject arg) {
	jclass cls = env->GetObjectClass(obj);
	jmethodID m = env->GetMethodID(cls, name, sig);
	env->DeleteLocalRef(cls);
	if (m == 0 || ClearException(env)) return 0;
	jobject r = (arg != 0) ? env->CallObjectMethod(obj, m, arg) : env->CallObjectMethod(obj, m);
	if (ClearException(env)) return 0;
	return r;
}

bool CallVoid(JNIEnv *env, jobject obj, const char *name, const char *sig, bool useArg, jboolean arg) {
	jclass cls = env->GetObjectClass(obj);
	jmethodID m = env->GetMethodID(cls, name, sig);
	env->DeleteLocalRef(cls);
	if (m == 0 || ClearException(env)) return false;
	if (useArg) {
		env->CallVoidMethod(obj, m, arg);
	} else {
		env->CallVoidMethod(obj, m);
	}
	return !ClearException(env);
}

void AcquireMulticastLock() {
	std::lock_guard<std::mutex> guard(g_lockMutex);
	if (g_multicastLock != 0) return;
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	jobject activity = (jobject)SDL_AndroidGetActivity();
	if (env == 0 || activity == 0) return;
	jobject app = CallObject(env, activity, "getApplicationContext", "()Landroid/content/Context;", 0);
	env->DeleteLocalRef(activity);
	if (app == 0) return;
	jstring wifiName = env->NewStringUTF("wifi");
	jobject wifi =
	    CallObject(env, app, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;", wifiName);
	env->DeleteLocalRef(wifiName);
	env->DeleteLocalRef(app);
	if (wifi == 0) return;
	jstring tag = env->NewStringUTF("sdlcastg");
	jobject lock = CallObject(env, wifi, "createMulticastLock",
	                          "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;", tag);
	env->DeleteLocalRef(tag);
	env->DeleteLocalRef(wifi);
	if (lock == 0) return;
	// 権限が無ければ acquire が SecurityException を投げる（ロックなしで続ける）。
	if (CallVoid(env, lock, "setReferenceCounted", "(Z)V", true, JNI_FALSE) &&
	    CallVoid(env, lock, "acquire", "()V", false, JNI_FALSE)) {
		g_multicastLock = env->NewGlobalRef(lock);
		SDL_Log("sdlcastg: multicast lock acquired");
	} else {
		SDL_Log("sdlcastg: cannot acquire multicast lock (CHANGE_WIFI_MULTICAST_STATE?)");
	}
	env->DeleteLocalRef(lock);
}

void ReleaseMulticastLock() {
	std::lock_guard<std::mutex> guard(g_lockMutex);
	if (g_multicastLock == 0) return;
	JNIEnv *env = (JNIEnv *)SDL_AndroidGetJNIEnv();
	if (env == 0) return;
	CallVoid(env, g_multicastLock, "release", "()V", false, JNI_FALSE);
	env->DeleteGlobalRef(g_multicastLock);
	g_multicastLock = 0;
	SDL_Log("sdlcastg: multicast lock released");
}
#endif

}  // namespace

extern "C" {

int SDLCastG_StartDiscoverySDL(void) {
#ifdef __ANDROID__
	AcquireMulticastLock();
#endif
	return SDLCastG_StartDiscovery();
}

void SDLCastG_StopDiscoverySDL(void) {
	SDLCastG_StopDiscovery();
#ifdef __ANDROID__
	ReleaseMulticastLock();
#endif
}

int SDLCastG_SubmitVideoFromRenderer(SDL_Renderer *renderer, int64_t ptsMs) {
	return SDLCastG_SubmitVideoFromRendererRect(renderer, 0, ptsMs);
}

int SDLCastG_SubmitVideoFromRendererRect(SDL_Renderer *renderer, const SDL_Rect *rect,
                                         int64_t ptsMs) {
	if (renderer == 0) return -1;
	if (!SDLCastG_WantVideoFrame()) return 0;

	// 描画先の大きさ。テクスチャに描いているならその大きさ。
	int w = 0, h = 0;
	SDL_Texture *target = SDL_GetRenderTarget(renderer);
	if (target != 0) {
		SDL_QueryTexture(target, 0, 0, &w, &h);
	} else if (SDL_GetRendererOutputSize(renderer, &w, &h) != 0) {
		return -1;
	}
	if (w <= 0 || h <= 0) return -1;
	// 読む範囲を描画先の中に収める。
	SDL_Rect r = { 0, 0, w, h };
	if (rect != 0) {
		const SDL_Rect all = { 0, 0, w, h };
		if (!SDL_IntersectRect(rect, &all, &r)) return -1;
	}
	w = r.w;
	h = r.h;

	std::vector<uint8_t> &buf = PixelBuffer();
	buf.resize((size_t)w * h * 4);
	// SDL_PIXELFORMAT_RGBA32 は「メモリ上で R, G, B, A の順」（エンディアンに
	// よらない別名）。
	if (SDL_RenderReadPixels(renderer, &r, SDL_PIXELFORMAT_RGBA32, &buf[0], w * 4) != 0) return -1;
	SDLCastG_SubmitVideoRGBA(&buf[0], w, h, w * 4, ptsMs);
	return 1;
}

void SDLCastG_ResetRendererTextures(void) {
	FrameTextures &t = Tex();
	DestroyTex(&t.frame);
	DestroyTex(&t.mid);
	DestroyTex(&t.small);
	t.active = false;
}

int SDLCastG_BeginRendererFrame(SDL_Renderer *renderer) {
	FrameTextures &t = Tex();
	t.active = false;
	if (renderer == 0) return -1;
	if (!SDLCastG_GetStreamSize(0, 0)) {
		// 流していなければ、持っているテクスチャは手放す（メモリを空ける）。
		if (t.frame != 0) SDLCastG_ResetRendererTextures();
		return 0;
	}
	if (t.renderer != renderer) {
		SDLCastG_ResetRendererTextures();
		t.renderer = renderer;
	}
	int w = 0, h = 0;
	if (SDL_GetRenderTarget(renderer) != 0 || SDL_GetRendererOutputSize(renderer, &w, &h) != 0 ||
	    w <= 0 || h <= 0) {
		return -1;
	}
	if (!EnsureTarget(renderer, &t.frame, &t.frameW, &t.frameH, w, h)) return -1;
	if (SDL_SetRenderTarget(renderer, t.frame) != 0) return -1;
	t.active = true;
	return 0;
}

void SDLCastG_EndRendererFrameNoVideo(SDL_Renderer *renderer) {
	FrameTextures &t = Tex();
	if (!t.active || renderer == 0) return;
	t.active = false;
	SDL_SetRenderTarget(renderer, 0);
	SDL_RenderCopy(renderer, t.frame, 0, 0);
}

int SDLCastG_EndRendererFrame(SDL_Renderer *renderer, const SDL_Rect *rect, int64_t ptsMs) {
	FrameTextures &t = Tex();
	if (!t.active || renderer == 0) return 0;
	t.active = false;
	int result = 0;
	int sw = 0, sh = 0;
	if (SDLCastG_WantVideoFrame() && SDLCastG_GetStreamSize(&sw, &sh)) {
		SDL_Rect src = { 0, 0, t.frameW, t.frameH };
		if (rect != 0) {
			const SDL_Rect all = src;
			if (!SDL_IntersectRect(rect, &all, &src)) src.w = 0;
		}
		if (src.w > 0 && src.h > 0) {
			// 送る大きさへ縦横比を保って収めた大きさ（偶数）。
			const double scale = SDL_min((double)sw / src.w, (double)sh / src.h);
			const int fw = SDL_max(2, SDL_min(sw, (int)(src.w * scale + 0.5)) & ~1);
			const int fh = SDL_max(2, SDL_min(sh, (int)(src.h * scale + 0.5)) & ~1);
			// 2 倍より大きく縮めるときは、いったん 2 倍の大きさへ縮めてから半分にする
			// （双線形で 1/2 にすると 2x2 の平均になり、細い字がちらつきにくい）。
			bool ok = EnsureTarget(renderer, &t.small, &t.smallW, &t.smallH, fw, fh);
			SDL_Texture *from = t.frame;
			const SDL_Rect *fromRect = &src;
			if (ok && (src.w > fw * 2 || src.h > fh * 2)) {
				ok = EnsureTarget(renderer, &t.mid, &t.midW, &t.midH, fw * 2, fh * 2) &&
				     SDL_SetRenderTarget(renderer, t.mid) == 0 &&
				     SDL_RenderCopy(renderer, t.frame, &src, 0) == 0;
				from = t.mid;
				fromRect = 0;
			}
			if (ok && SDL_SetRenderTarget(renderer, t.small) == 0 &&
			    SDL_RenderCopy(renderer, from, fromRect, 0) == 0) {
				std::vector<uint8_t> &buf = PixelBuffer();
				buf.resize((size_t)fw * fh * 4);
				if (SDL_RenderReadPixels(renderer, 0, SDL_PIXELFORMAT_RGBA32, &buf[0], fw * 4) == 0) {
					SDLCastG_SubmitVideoRGBA(&buf[0], fw, fh, fw * 4, ptsMs);
					result = 1;
				} else {
					result = -1;
				}
			} else {
				result = -1;
			}
		}
	}
	// 窓へ写す（このあと呼ぶ側が SDL_RenderPresent する）。
	SDL_SetRenderTarget(renderer, 0);
	SDL_RenderCopy(renderer, t.frame, 0, 0);
	return result;
}

int SDLCastG_SubmitAudioSDL(const void *data, int bytes, const SDL_AudioSpec *spec) {
	if (data == 0 || bytes <= 0 || spec == 0) return -1;

	// すでに 48kHz ステレオの int16 なら、そのまま。
	if (spec->format == AUDIO_S16SYS && spec->channels == 2 && spec->freq == 48000) {
		return SDLCastG_SubmitAudio((const int16_t *)data, bytes / 4);
	}

	AudioConv &c = Conv();
	std::lock_guard<std::mutex> lock(c.mutex);
	if (c.stream == 0 || c.format != spec->format || c.channels != spec->channels ||
	    c.freq != spec->freq) {
		if (c.stream != 0) SDL_FreeAudioStream(c.stream);
		c.stream = SDL_NewAudioStream(spec->format, spec->channels, spec->freq, AUDIO_S16SYS, 2,
		                              48000);
		if (c.stream == 0) return -1;
		c.format = spec->format;
		c.channels = spec->channels;
		c.freq = spec->freq;
	}
	if (SDL_AudioStreamPut(c.stream, data, bytes) != 0) return -1;
	const int avail = SDL_AudioStreamAvailable(c.stream);
	if (avail <= 0) return 0;
	c.out.resize((size_t)avail / 2 + 2);
	const int got = SDL_AudioStreamGet(c.stream, &c.out[0], avail);
	if (got <= 0) return 0;
	return SDLCastG_SubmitAudio(&c.out[0], got / 4);
}

}  // extern "C"
