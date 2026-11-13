// tls.h - the head unit's TLS 1.2 client (mbedTLS) over memory, no socket.
//
// Android Auto tunnels the handshake through control messages
// (ENCAPSULATED_SSL) and afterwards sends each encrypted frame's payload as
// TLS records. So this class only turns bytes into bytes:
//   handshake(): peer handshake bytes in, ours out, until done
//   encrypt():   plaintext in, records out
//   decrypt():   records in, plaintext out
//
// The head unit is the client and authenticates with its certificate; the
// phone's certificate is not checked (aasdk doesn't either).
#ifndef ANDROID_AUTO_TLS_H
#define ANDROID_AUTO_TLS_H

#include <cstddef>
#include "types.h"
#include <memory>
#include <string>
#include <vector>

namespace aa {

class Tls {
public:
	enum class Step { More, Done, Failed };

	Tls();
	~Tls();
	Tls(const Tls &) = delete;
	Tls &operator=(const Tls &) = delete;

	// cert_pem / key_pem: the head unit's certificate and private key.
	// false (with *err) if they don't parse or mbedTLS won't set up.
	bool init(const std::string &cert_pem, const std::string &key_pem, std::string *err);

	// Feed the phone's handshake bytes (none for the first call); appends
	// what to send back to out, possibly nothing.
	Step handshake(const u8 *in, usize n, std::vector<u8> &out);
	bool done() const;

	// Appends the TLS records for p[0..n) to out.
	bool encrypt(const u8 *p, usize n, std::vector<u8> &out);
	// Appends the plaintext of whatever complete records p[0..n) finishes
	// (a record may span calls) to out.
	bool decrypt(const u8 *p, usize n, std::vector<u8> &out);

	// The last mbedTLS error as text, and the negotiated suite.
	const std::string &error() const { return error_; }
	std::string suite() const;

	// mbedTLS's own trace (handshake steps) at this level to log_msg: 0
	// off, 1 errors, 2 state changes, 3+ verbose.
	static void set_debug(int level);

private:
	struct Impl;
	std::unique_ptr<Impl> d_;
	std::string           error_;

	void set_error(const char *what, int rc);
};

// Entropy for the random generator; platform-specific (tls.cpp). The unit
// has no /dev/random.
int entropy_source(void *data, unsigned char *out, usize len, usize *olen);

} // namespace aa

#endif
