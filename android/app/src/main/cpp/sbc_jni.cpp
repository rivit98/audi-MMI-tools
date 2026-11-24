// sbc_jni.cpp - JNI for mmi3g.audistreaming.SbcNative: BlueZ's SBC encoder
// (third_party/sbc-*, fetched by the top Makefile) for the phone's audio to
// the MMI (PhoneAudio.kt, proto.h AUDIO_*). One encoder per handle; the
// caller keeps each handle on one thread.
//
// Settings: 44.1 kHz, joint stereo, 16 blocks, 8 subbands, loudness, the
// bitpool given (53 = A2DP's "high quality", 119-byte frames, ~328 kbit/s).
// PCM in: 16-bit little endian, interleaved stereo, whole frames of
// codeSize() bytes (128 samples x 2 channels x 2 bytes = 512).
#include <jni.h>

#include <vector>

#include "types.h"

extern "C" {
#include "sbc/sbc.h"
}

namespace {

sbc_t *enc(jlong h)
{
	return reinterpret_cast<sbc_t *>(h);  // types-ok: a handle is a pointer
}

} // namespace

extern "C" JNIEXPORT jlong JNICALL
Java_mmi3g_audistreaming_SbcNative_open(JNIEnv *, jclass, jint bitpool)
{
	auto *s = new sbc_t;
	if (sbc_init(s, 0) != 0) {
		delete s;
		return 0;
	}
	s->frequency = SBC_FREQ_44100;
	s->mode = SBC_MODE_JOINT_STEREO;
	s->allocation = SBC_AM_LOUDNESS;
	s->subbands = SBC_SB_8;
	s->blocks = SBC_BLK_16;
	s->bitpool = (u8)bitpool;
	s->endian = SBC_LE;
	return reinterpret_cast<jlong>(s);
}

extern "C" JNIEXPORT jint JNICALL
Java_mmi3g_audistreaming_SbcNative_codeSize(JNIEnv *, jclass, jlong h)
{
	return h ? (jint)sbc_get_codesize(enc(h)) : 0;
}

extern "C" JNIEXPORT jint JNICALL
Java_mmi3g_audistreaming_SbcNative_frameLength(JNIEnv *, jclass, jlong h)
{
	return h ? (jint)sbc_get_frame_length(enc(h)) : 0;
}

// Encodes pcm[0, n) (whole frames of codeSize bytes) into out; returns the
// bytes written (whole frames), or -1 if out is too small or the encoder
// refused.
extern "C" JNIEXPORT jint JNICALL
Java_mmi3g_audistreaming_SbcNative_encode(JNIEnv *env, jclass, jlong h, jbyteArray pcm, jint n,
                                          jbyteArray out)
{
	if (!h || n < 0 || n > env->GetArrayLength(pcm))
		return -1;
	sbc_t *s = enc(h);
	usize code = sbc_get_codesize(s);
	usize flen = sbc_get_frame_length(s);
	usize frames = (usize)n / code;
	usize cap = (usize)env->GetArrayLength(out);
	if (frames * flen > cap)
		return -1;
	std::vector<u8> in((usize)n), o(frames * flen);
	env->GetByteArrayRegion(pcm, 0, n, reinterpret_cast<jbyte *>(in.data()));
	usize at = 0;
	for (usize f = 0; f < frames; f++) {
		isize written = 0;
		isize used = sbc_encode(s, in.data() + f * code, code, o.data() + at, o.size() - at, &written);
		if (used != (isize)code || written <= 0)
			return -1;
		at += (usize)written;
	}
	env->SetByteArrayRegion(out, 0, (jsize)at, reinterpret_cast<const jbyte *>(o.data()));
	return (jint)at;
}

extern "C" JNIEXPORT void JNICALL
Java_mmi3g_audistreaming_SbcNative_close(JNIEnv *, jclass, jlong h)
{
	if (!h)
		return;
	sbc_finish(enc(h));
	delete enc(h);
}
