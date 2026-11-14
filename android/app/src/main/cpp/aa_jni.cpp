// aa_jni.cpp - JNI for mmi3g.audistreaming.AaNative: one Android Auto head
// unit session (src/android_auto/session.h) inside the phone app.
//
// The session is single-threaded: AaService calls every native method from
// its "aa" thread, and the session's Host callbacks (bytes to send, video,
// audio, phase changes) come back on that thread, during those calls, into
// the Kotlin callbacks object. So the JNIEnv of the current call is the one
// to call back with.
//
// The session's log_msg lines (src/common/log.c) are caught by a log tap
// into a small queue that AaService drains into its own log (drainLog).
// A Kotlin callback must not leave an exception pending: clear() logs and
// clears one, so the next JNI call stays legal.
#include <jni.h>

#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "session.h"
#include "types.h"

extern "C" {
#include "log.h"
}

namespace {

constexpr usize kMaxLogLines = 500;

std::mutex g_log_lock;
std::deque<std::string> g_log;
unsigned g_log_dropped = 0;

void log_tap(const char *line)
{
	std::lock_guard<std::mutex> g(g_log_lock);
	if (g_log.size() >= kMaxLogLines) {
		g_log.pop_front();
		g_log_dropped++;
	}
	g_log.emplace_back(line);
}

jbyteArray bytes(JNIEnv *env, const u8 *p, usize n)
{
	jbyteArray a = env->NewByteArray((jsize)n);
	if (a != nullptr && n > 0)
		env->SetByteArrayRegion(a, 0, (jsize)n, reinterpret_cast<const jbyte *>(p));
	return a;
}

// A callback that threw: the exception is logged and cleared, so the
// session's next JNI call is legal (the Kotlin side catches its own too).
void clear(JNIEnv *env, const char *what)
{
	if (!env->ExceptionCheck())
		return;
	env->ExceptionDescribe();
	env->ExceptionClear();
	log_msg("aa_jni: the %s callback threw (cleared)", what);
}

class JniHost : public aa::Host {
public:
	JniHost(JNIEnv *env, jobject cb) : cb_(env->NewGlobalRef(cb))
	{
		jclass c = env->GetObjectClass(cb);
		send_ = env->GetMethodID(c, "send", "([B)V");
		phase_ = env->GetMethodID(c, "phase", "(ILjava/lang/String;)V");
		video_ = env->GetMethodID(c, "video", "([BJZ)V");
		audio_ = env->GetMethodID(c, "audio", "(I[B)V");
		env->DeleteLocalRef(c);
	}

	void release(JNIEnv *env) { env->DeleteGlobalRef(cb_); }

	JNIEnv *env = nullptr;      // the current call's

	void send(const u8 *p, usize n) override
	{
		jbyteArray a = bytes(env, p, n);
		env->CallVoidMethod(cb_, send_, a);
		clear(env, "send");
		env->DeleteLocalRef(a);
	}

	void phase(aa::Phase p, const std::string &detail) override
	{
		jstring s = env->NewStringUTF(detail.c_str());
		env->CallVoidMethod(cb_, phase_, (jint)p, s);
		clear(env, "phase");
		env->DeleteLocalRef(s);
	}

	void video(const u8 *p, usize n, u64 ts_us, bool config) override
	{
		jbyteArray a = bytes(env, p, n);
		env->CallVoidMethod(cb_, video_, a, (jlong)ts_us, (jboolean)config);
		clear(env, "video");
		env->DeleteLocalRef(a);
	}

	void audio(u8 channel, const u8 *p, usize n) override
	{
		jbyteArray a = bytes(env, p, n);
		env->CallVoidMethod(cb_, audio_, (jint)channel, a);
		clear(env, "audio");
		env->DeleteLocalRef(a);
	}

	unsigned now_ms() override { return log_ms(); }

private:
	jobject   cb_;
	jmethodID send_, phase_, video_, audio_;
};

struct Native {
	JniHost      host;
	aa::Session  session;
	Native(JNIEnv *env, jobject cb, const aa::Config &cfg) : host(env, cb), session(cfg, host) {}
};

Native *from(jlong h)
{
	return reinterpret_cast<Native *>(h);
}

// A call into the session: the host calls back with this env. A Java
// exception thrown by a callback is left pending for the Kotlin caller.
Native *enter(JNIEnv *env, jlong h)
{
	Native *n = from(h);
	n->host.env = env;
	return n;
}

std::string text(JNIEnv *env, jstring s)
{
	const char *c = env->GetStringUTFChars(s, nullptr);
	std::string r = c ? c : "";
	if (c)
		env->ReleaseStringUTFChars(s, c);
	return r;
}

} // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_mmi3g_audistreaming_AaNative_nativeCreate(JNIEnv *env, jclass, jobject cb,
    jboolean audio, jint margin_w, jint margin_h, jint video_fps, jstring name)
{
	log_set_tap(log_tap);
	aa::Config cfg;
	cfg.audio = audio;
	cfg.margin_w = (unsigned)margin_w;
	cfg.margin_h = (unsigned)margin_h;
	cfg.video_fps = (unsigned)video_fps;
	cfg.display_name = text(env, name);
	return reinterpret_cast<jlong>(new Native(env, cb, cfg));
}

JNIEXPORT jboolean JNICALL Java_mmi3g_audistreaming_AaNative_nativeStart(JNIEnv *env, jclass, jlong h,
    jstring cert, jstring key)
{
	return enter(env, h)->session.start(text(env, cert), text(env, key));
}

JNIEXPORT void JNICALL Java_mmi3g_audistreaming_AaNative_nativeRx(JNIEnv *env, jclass, jlong h,
    jbyteArray data, jint n)
{
	Native *nat = enter(env, h);
	jbyte *p = env->GetByteArrayElements(data, nullptr);
	if (p == nullptr)
		return;
	// a copy: the callbacks may allocate, and the elements must not stay pinned
	std::vector<u8> buf(reinterpret_cast<u8 *>(p), reinterpret_cast<u8 *>(p) + n);
	env->ReleaseByteArrayElements(data, p, JNI_ABORT);
	nat->session.rx(buf.data(), buf.size());
}

JNIEXPORT jint JNICALL Java_mmi3g_audistreaming_AaNative_nativeTick(JNIEnv *env, jclass, jlong h, jint limit_ms)
{
	Native *n = enter(env, h);
	n->session.tick();
	return (jint)n->session.ms_to_next((unsigned)limit_ms);
}

JNIEXPORT void JNICALL Java_mmi3g_audistreaming_AaNative_nativeTouch(JNIEnv *env, jclass, jlong h,
    jint action, jint index, jintArray xs, jintArray ys, jintArray ids)
{
	Native *n = enter(env, h);
	jsize count = env->GetArrayLength(xs);
	std::vector<jint> x(count), y(count), id(count);
	env->GetIntArrayRegion(xs, 0, count, x.data());
	env->GetIntArrayRegion(ys, 0, count, y.data());
	env->GetIntArrayRegion(ids, 0, count, id.data());
	std::vector<aa::Pointer> p(count);
	for (jsize i = 0; i < count; i++)
		p[i] = {(unsigned)x[i], (unsigned)y[i], (unsigned)id[i]};
	n->session.touch((aa::Touch)action, (unsigned)index, p.data(), p.size());
}

JNIEXPORT void JNICALL Java_mmi3g_audistreaming_AaNative_nativeBye(JNIEnv *env, jclass, jlong h)
{
	enter(env, h)->session.bye();
}

JNIEXPORT jint JNICALL Java_mmi3g_audistreaming_AaNative_nativePhase(JNIEnv *, jclass, jlong h)
{
	return (jint)from(h)->session.phase();
}

// video frames, video bytes, rx bytes, tx bytes, pings sent, pongs, last rtt
// ms, handshake ms, touches, audio bytes (media, guidance, system)
JNIEXPORT jlongArray JNICALL Java_mmi3g_audistreaming_AaNative_nativeStats(JNIEnv *env, jclass, jlong h)
{
	const aa::Stats &s = from(h)->session.stats();
	jlong v[] = {(jlong)s.video_frames, (jlong)s.video_bytes, (jlong)s.rx_bytes, (jlong)s.tx_bytes,
	             (jlong)s.pings_sent, (jlong)s.pongs, (jlong)s.last_rtt_ms, (jlong)s.handshake_ms,
	             (jlong)s.touches, (jlong)s.audio_bytes[0], (jlong)s.audio_bytes[1],
	             (jlong)s.audio_bytes[2]};
	jlongArray a = env->NewLongArray((jsize)(sizeof(v) / sizeof(v[0])));
	if (a != nullptr)
		env->SetLongArrayRegion(a, 0, (jsize)(sizeof(v) / sizeof(v[0])), v);
	return a;
}

// 0: the phone's name, 1: the TLS suite, 2: the error (Failed)
JNIEXPORT jstring JNICALL Java_mmi3g_audistreaming_AaNative_nativeText(JNIEnv *env, jclass, jlong h, jint which)
{
	const aa::Stats &s = from(h)->session.stats();
	const std::string &t = which == 0 ? s.phone : which == 1 ? s.suite : s.error;
	return env->NewStringUTF(t.c_str());
}

JNIEXPORT void JNICALL Java_mmi3g_audistreaming_AaNative_nativeDestroy(JNIEnv *env, jclass, jlong h)
{
	Native *n = from(h);
	n->host.release(env);
	delete n;
}

// The session's log lines since the last call ("[   123 ms] aa: ..."), and
// how many were dropped because nobody drained them.
JNIEXPORT jobjectArray JNICALL Java_mmi3g_audistreaming_AaNative_nativeDrainLog(JNIEnv *env, jclass)
{
	std::deque<std::string> lines;
	unsigned dropped;
	{
		std::lock_guard<std::mutex> g(g_log_lock);
		lines.swap(g_log);
		dropped = g_log_dropped;
		g_log_dropped = 0;
	}
	if (dropped > 0)
		lines.push_front(std::to_string(dropped) + " log lines dropped");
	jclass str = env->FindClass("java/lang/String");
	jobjectArray a = env->NewObjectArray((jsize)lines.size(), str, nullptr);
	for (usize i = 0; a != nullptr && i < lines.size(); i++) {
		jstring s = env->NewStringUTF(lines[i].c_str());
		env->SetObjectArrayElement(a, (jsize)i, s);
		env->DeleteLocalRef(s);
	}
	return a;
}

} // extern "C"
