// tls.cpp - see tls.h.
#include "tls.h"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/debug.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#ifdef __QNX__
#include <sys/neutrino.h>
#else
#include <time.h>
#endif

extern "C" {
#include "log.h"
}

namespace aa {

namespace {

constexpr size_t kReadChunk = 16384;

// /dev/urandom's last answer: bytes read, or -errno (-1 = not tried yet)
int urandom_got = -1;

uint64_t cycles()
{
#ifdef __QNX__
	// On SH, ClockCycles() returns the TMU1 count shifted up by
	// CLOCKCYCLES_INCR_BIT (32): its low 32 bits are always 0.
#ifdef CLOCKCYCLES_INCR_BIT
	return ClockCycles() >> CLOCKCYCLES_INCR_BIT;
#else
	return ClockCycles();
#endif
#else
	timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
#endif
}

void debug_cb(void *, int level, const char *file, int line, const char *msg)
{
	const char *base = std::strrchr(file, '/');
	size_t n = std::strlen(msg);
	if (n > 0 && msg[n - 1] == '\n')
		n--;
	log_msg("tls[%d] %s:%d %.*s", level, base ? base + 1 : file, line, (int)n, msg);
}

} // namespace

// The unit has no /dev/random (sloginfo: "Using pseudo random generator"),
// so the random bytes come from the timing jitter of the CPU's cycle
// counter around system calls, mixed with whatever /dev/urandom or
// /dev/random gives where there is one. CTR-DRBG's seed takes it directly
// (seed_source), which runs it through AES before use. Weak, but what it protects is
// a USB cable inside the car; the session keys also get the phone's
// randomness through ECDHE.
int entropy_source(void *, unsigned char *out, size_t len, size_t *olen)
{
	size_t got = 0;
	for (const char *dev : {"/dev/urandom", "/dev/random"}) {
		int fd = open(dev, O_RDONLY | O_NONBLOCK);
		if (fd < 0) {
			urandom_got = -errno;
			continue;
		}
		ssize_t r = read(fd, out, len);
		urandom_got = r < 0 ? -errno : (int)r;
		close(fd);
		if (r > 0)
			got = (size_t)r;
		break;
	}
	for (size_t i = 0; i < len; i++) {
		uint8_t acc = 0;
		for (int k = 0; k < 8; k++) {
			uint64_t t0 = cycles();
			getpid();                        // a kernel call: its time varies
			uint64_t t1 = cycles();
			acc = (uint8_t)((acc << 1 | acc >> 7) ^ (uint8_t)(t1 - t0) ^ (uint8_t)(t1 >> 3));
		}
		out[i] = i < got ? (uint8_t)(out[i] ^ acc) : acc;
	}
	*olen = len;
	return 0;
}

// CTR-DRBG's seed straight from entropy_source, without mbedTLS's entropy
// pool: on the unit mbedtls_entropy_func failed (CTR-DRBG "entropy source
// failed") while the same code passes on a PC and under qemu-sh4.
static int seed_source(void *, unsigned char *out, size_t len)
{
	while (len > 0) {
		unsigned char buf[MBEDTLS_ENTROPY_MAX_GATHER];
		size_t n = len < sizeof(buf) ? len : sizeof(buf), got = 0;
		if (entropy_source(nullptr, buf, n, &got) != 0 || got == 0)
			return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
		if (got > n)
			got = n;
		std::memcpy(out, buf, got);
		out += got;
		len -= got;
	}
	return 0;
}

struct Tls::Impl {
	mbedtls_ssl_context      ssl;
	mbedtls_ssl_config       conf;
	mbedtls_entropy_context  entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt         cert;
	mbedtls_pk_context       key;

	std::vector<uint8_t>  in;       // from the phone, not yet read by mbedTLS
	size_t                in_off = 0;
	std::vector<uint8_t> *out = nullptr;   // where mbedTLS writes, during a call
	bool                  done = false;

	Impl()
	{
		mbedtls_ssl_init(&ssl);
		mbedtls_ssl_config_init(&conf);
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		mbedtls_x509_crt_init(&cert);
		mbedtls_pk_init(&key);
	}
	~Impl()
	{
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
		mbedtls_x509_crt_free(&cert);
		mbedtls_pk_free(&key);
	}

	static int send_cb(void *ctx, const unsigned char *buf, size_t len)
	{
		Impl *d = static_cast<Impl *>(ctx);
		if (d->out == nullptr)
			return MBEDTLS_ERR_SSL_WANT_WRITE;
		d->out->insert(d->out->end(), buf, buf + len);
		return (int)len;
	}
	static int recv_cb(void *ctx, unsigned char *buf, size_t len)
	{
		Impl *d = static_cast<Impl *>(ctx);
		size_t avail = d->in.size() - d->in_off;
		if (avail == 0)
			return MBEDTLS_ERR_SSL_WANT_READ;
		size_t n = len < avail ? len : avail;
		std::memcpy(buf, d->in.data() + d->in_off, n);
		d->in_off += n;
		if (d->in_off == d->in.size()) {
			d->in.clear();
			d->in_off = 0;
		}
		return (int)n;
	}
	void feed(const uint8_t *p, size_t n)
	{
		if (in_off > 0 && in_off > in.size() / 2) {
			in.erase(in.begin(), in.begin() + (ptrdiff_t)in_off);
			in_off = 0;
		}
		in.insert(in.end(), p, p + n);
	}
};

Tls::Tls() : d_(new Impl) {}
Tls::~Tls() = default;

void Tls::set_debug(int level)
{
	mbedtls_debug_set_threshold(level);
}

void Tls::set_error(const char *what, int rc)
{
	char buf[160];
	mbedtls_strerror(rc, buf, sizeof(buf));
	char line[256];
	std::snprintf(line, sizeof(line), "%s: -0x%04x %s", what, (unsigned)-rc, buf);
	error_ = line;
}

bool Tls::init(const std::string &cert_pem, const std::string &key_pem, std::string *err)
{
	static const char kPers[] = "mmi3g android_auto";
	// Diagnostic only: what mbedTLS's entropy pool says on this system. The
	// seed below doesn't use it.
	int rc = mbedtls_entropy_add_source(&d_->entropy, entropy_source, nullptr, 32,
	                                    MBEDTLS_ENTROPY_SOURCE_STRONG);
	if (rc == 0) {
		unsigned char probe[32];
		rc = mbedtls_entropy_func(&d_->entropy, probe, sizeof(probe));
		mbedtls_platform_zeroize(probe, sizeof(probe));
	}
	{
		char buf[120] = "ok";
		if (rc != 0)
			mbedtls_strerror(rc, buf, sizeof(buf));
		log_msg("tls: entropy pool: -0x%04x %s; /dev/urandom: %d", (unsigned)-rc, buf, urandom_got);
	}
	rc = mbedtls_ctr_drbg_seed(&d_->drbg, seed_source, nullptr,
	                           reinterpret_cast<const unsigned char *>(kPers), sizeof(kPers) - 1);
	if (rc != 0) {
		set_error("random generator", rc);
		goto fail;
	}
	// PEM parsing wants the terminating NUL counted
	rc = mbedtls_x509_crt_parse(&d_->cert, reinterpret_cast<const unsigned char *>(cert_pem.c_str()),
	                            cert_pem.size() + 1);
	if (rc != 0) {
		set_error("certificate", rc);
		goto fail;
	}
	rc = mbedtls_pk_parse_key(&d_->key, reinterpret_cast<const unsigned char *>(key_pem.c_str()),
	                          key_pem.size() + 1, nullptr, 0, mbedtls_ctr_drbg_random, &d_->drbg);
	if (rc != 0) {
		set_error("private key", rc);
		goto fail;
	}
	rc = mbedtls_ssl_config_defaults(&d_->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
	                                 MBEDTLS_SSL_PRESET_DEFAULT);
	if (rc != 0) {
		set_error("ssl config", rc);
		goto fail;
	}
	mbedtls_ssl_conf_authmode(&d_->conf, MBEDTLS_SSL_VERIFY_NONE);
	mbedtls_ssl_conf_rng(&d_->conf, mbedtls_ctr_drbg_random, &d_->drbg);
	mbedtls_ssl_conf_dbg(&d_->conf, debug_cb, nullptr);
	mbedtls_ssl_conf_min_tls_version(&d_->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	mbedtls_ssl_conf_max_tls_version(&d_->conf, MBEDTLS_SSL_VERSION_TLS1_2);
	rc = mbedtls_ssl_conf_own_cert(&d_->conf, &d_->cert, &d_->key);
	if (rc == 0)
		rc = mbedtls_ssl_setup(&d_->ssl, &d_->conf);
	if (rc != 0) {
		set_error("ssl setup", rc);
		goto fail;
	}
	mbedtls_ssl_set_bio(&d_->ssl, d_.get(), Impl::send_cb, Impl::recv_cb, nullptr);
	return true;
fail:
	if (err)
		*err = error_;
	return false;
}

Tls::Step Tls::handshake(const uint8_t *in, size_t n, std::vector<uint8_t> &out)
{
	if (n > 0)
		d_->feed(in, n);
	d_->out = &out;
	int rc = mbedtls_ssl_handshake(&d_->ssl);
	d_->out = nullptr;
	if (rc == 0) {
		d_->done = true;
		return Step::Done;
	}
	if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE)
		return Step::More;
	set_error("handshake", rc);
	return Step::Failed;
}

bool Tls::done() const
{
	return d_->done;
}

std::string Tls::suite() const
{
	const char *s = mbedtls_ssl_get_ciphersuite(&d_->ssl);
	const char *v = mbedtls_ssl_get_version(&d_->ssl);
	return std::string(v ? v : "?") + " " + (s ? s : "?");
}

bool Tls::encrypt(const uint8_t *p, size_t n, std::vector<uint8_t> &out)
{
	d_->out = &out;
	size_t off = 0;
	while (off < n) {
		int rc = mbedtls_ssl_write(&d_->ssl, p + off, n - off);
		if (rc <= 0) {
			d_->out = nullptr;
			set_error("encrypt", rc);
			return false;
		}
		off += (size_t)rc;
	}
	d_->out = nullptr;
	return true;
}

bool Tls::decrypt(const uint8_t *p, size_t n, std::vector<uint8_t> &out)
{
	d_->feed(p, n);
	std::vector<uint8_t> dummy;
	d_->out = &dummy;                     // an alert we'd send: dropped
	for (;;) {
		size_t at = out.size();
		out.resize(at + kReadChunk);
		int rc = mbedtls_ssl_read(&d_->ssl, out.data() + at, kReadChunk);
		if (rc > 0) {
			out.resize(at + (size_t)rc);
			continue;
		}
		out.resize(at);
		d_->out = nullptr;
		if (rc == MBEDTLS_ERR_SSL_WANT_READ)
			return true;
		set_error("decrypt", rc == 0 ? MBEDTLS_ERR_SSL_CONN_EOF : rc);
		return false;
	}
}

} // namespace aa
