// test_android_auto.cpp - android_auto's protocol core on the build host:
// protobuf (pb.h), frames (frame.h), and a whole session (session.cpp,
// tls.cpp) against a fake phone that plays the phone's side: version
// response, TLS server (mbedTLS, built with the server side on the host),
// service discovery, channel open, video setup / start / data in fragments,
// ping, bye-bye.
//
// usage: test_android_auto [--pb DIR] [<headunit.crt> <headunit.key>]
// (default: third_party/aasdk-cert/, fetched by make)
//   --pb DIR  cross-check with Google's protobuf (tests/check_aa_proto.py,
//             make test): the fake phone sends the messages in DIR/phone/,
//             encoded by protobuf from aasdk's .proto files, and every
//             message the head unit sends goes to DIR/hu/ (index.txt:
//             "seq channel flags id") for check_aa_proto.py to decode.
//             Without it, the phone's messages come from pb.h's Writer.
// The fake phone reuses the head unit's certificate as its own.
#include "types.h"
#include "check.h"

#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "frame.h"
#include "pb.h"
#include "session.h"

namespace {

std::string slurp(const char *path)
{
	std::ifstream f(path, std::ios::binary);
	std::stringstream s;
	s << f.rdbuf();
	return s.str();
}

void test_pb()
{
	pb::Writer w, sub;
	sub.u(1, 800).u(2, 480);
	w.u(1, 3).i(2, -5).b(3, true).s(4, "hi").msg(5, sub).packed(6, {3, 65536});
	CHECK_EQ(w.buf[0], 0x08);
	u64 v = 0;
	CHECK(pb::get_u(w.buf.data(), w.buf.size(), 1, v));
	CHECK_EQ(v, 3);
	CHECK(pb::get_u(w.buf.data(), w.buf.size(), 2, v));
	CHECK_EQ((i32)v, -5);
	CHECK(pb::get_u(w.buf.data(), w.buf.size(), 3, v));
	CHECK_EQ(v, 1);
	CHECK(pb::get_str(w.buf.data(), w.buf.size(), 4) == "hi");
	// zigzag: -1 -> 1, 1 -> 2, -64 -> 127
	pb::Writer z;
	z.si(1, -1).si(2, 1).si(3, -64);
	CHECK(z.buf == std::vector<u8>({0x08, 1, 0x10, 2, 0x18, 127}));
	CHECK_EQ(pb::unzigzag(1), -1);
	CHECK_EQ(pb::unzigzag(127), -64);
	CHECK_EQ(pb::unzigzag(4294967294u), 2147483647);
	const u8 *p;
	usize n;
	CHECK(pb::get_bytes(w.buf.data(), w.buf.size(), 5, p, n));
	CHECK(pb::get_u(p, n, 2, v));
	CHECK_EQ(v, 480);
	// -5 as int32 is a 10-byte varint
	pb::Writer neg;
	neg.i(1, -5);
	CHECK_EQ(neg.buf.size(), 11);
	// malformed: length past the end
	const u8 bad[] = {0x22, 0x05, 'a'};
	pb::Reader r(bad, sizeof(bad));
	pb::Field f;
	CHECK(!r.next(f));
	CHECK(!r.ok());
}

void test_frames()
{
	// encode: bulk has a short header, FIRST an extended one
	std::vector<u8> out;
	const u8 pay[] = {1, 2, 3};
	aap::put_frame(out, 3, aap::kBulk | aap::kEncrypted, 3, pay, 3);
	CHECK_EQ(out.size(), 7);
	CHECK_EQ(out[1], 0x0b);
	aap::put_frame(out, 3, aap::kFirst, 9, pay, 3);
	CHECK_EQ(out.size(), 7 + 11);
	aap::put_frame(out, 3, aap::kLast, 0, pay, 3);

	// decode byte by byte
	aap::Decoder d;
	std::vector<aap::Frame> got;
	std::vector<u32> totals;
	for (u8 b : out)
		d.feed(&b, 1, [&](const aap::Frame &f) {
			got.push_back(f);
			totals.push_back(f.total);
		});
	CHECK_EQ(got.size(), 3);
	CHECK_EQ(totals[1], 9);
	CHECK_EQ(d.pending(), 0);

	// reassembly: interleaved channels
	aap::Assembler a;
	CHECK(a.add(3, aap::kFirst, pay, 3) == aap::Assembler::Result::Partial);
	CHECK(a.add(4, aap::kBulk, pay, 2) == aap::Assembler::Result::Done);
	CHECK_EQ(a.message(4).size(), 2);
	CHECK(a.add(3, 0, pay, 3) == aap::Assembler::Result::Partial);
	CHECK(a.add(3, aap::kLast, pay, 1) == aap::Assembler::Result::Done);
	CHECK_EQ(a.message(3).size(), 7);
	CHECK(a.add(5, aap::kLast, pay, 1) == aap::Assembler::Result::Bad);

	// too big a frame
	const u8 huge[] = {0, 3, 0xff, 0xff};
	CHECK(d.feed(huge, 4, [](const aap::Frame &) {}) == aap::Decoder::Result::Bad);

	// USB chunks never a multiple of the packet size
	CHECK_EQ(usb_chunk(512, 16384, 512), 511);
	CHECK_EQ(usb_chunk(1, 16384, 512), 1);
	CHECK_EQ(usb_chunk(100, 16384, 512), 100);
	CHECK_EQ(usb_chunk(40000, 16384, 512), 16383);
	CHECK_EQ(usb_chunk(1024, 16384, 0), 1024);
}

// --- the fake phone ------------------------------------------------------

struct TlsServer {
	mbedtls_ssl_context      ssl;
	mbedtls_ssl_config       conf;
	mbedtls_entropy_context  entropy;
	mbedtls_ctr_drbg_context drbg;
	mbedtls_x509_crt         cert;
	mbedtls_pk_context       key;
	std::vector<u8>     in, *out = nullptr;
	bool                     done = false;

	static int send_cb(void *ctx, const unsigned char *b, usize n)
	{
		auto *s = static_cast<TlsServer *>(ctx);
		s->out->insert(s->out->end(), b, b + n);
		return (int)n;
	}
	static int recv_cb(void *ctx, unsigned char *b, usize n)
	{
		auto *s = static_cast<TlsServer *>(ctx);
		if (s->in.empty())
			return MBEDTLS_ERR_SSL_WANT_READ;
		n = n < s->in.size() ? n : s->in.size();
		std::memcpy(b, s->in.data(), n);
		s->in.erase(s->in.begin(), s->in.begin() + (isize)n);
		return (int)n;
	}

	bool init(const std::string &crt, const std::string &k)
	{
		mbedtls_ssl_init(&ssl);
		mbedtls_ssl_config_init(&conf);
		mbedtls_entropy_init(&entropy);
		mbedtls_ctr_drbg_init(&drbg);
		mbedtls_x509_crt_init(&cert);
		mbedtls_pk_init(&key);
		mbedtls_entropy_add_source(&entropy, aa::entropy_source, nullptr, 32,
		                           MBEDTLS_ENTROPY_SOURCE_STRONG);
		return mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, nullptr, 0) == 0 &&
		       mbedtls_x509_crt_parse(&cert, (const unsigned char *)crt.c_str(), crt.size() + 1) == 0 &&
		       mbedtls_pk_parse_key(&key, (const unsigned char *)k.c_str(), k.size() + 1, nullptr, 0,
		                            mbedtls_ctr_drbg_random, &drbg) == 0 &&
		       mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER, MBEDTLS_SSL_TRANSPORT_STREAM,
		                                   MBEDTLS_SSL_PRESET_DEFAULT) == 0 &&
		       (mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg),
		        // asks for the client certificate; its chain (Google's CA) isn't here
		        mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL),
		        mbedtls_ssl_conf_own_cert(&conf, &cert, &key) == 0) &&
		       mbedtls_ssl_setup(&ssl, &conf) == 0 &&
		       (mbedtls_ssl_set_bio(&ssl, this, send_cb, recv_cb, nullptr), true);
	}
	~TlsServer()
	{
		mbedtls_ssl_free(&ssl);
		mbedtls_ssl_config_free(&conf);
		mbedtls_ctr_drbg_free(&drbg);
		mbedtls_entropy_free(&entropy);
		mbedtls_x509_crt_free(&cert);
		mbedtls_pk_free(&key);
	}
};

class Phone : public aa::Host {
public:
	aa::Session *hu = nullptr;
	TlsServer    tls;
	aap::Decoder dec;
	aap::Assembler as;
	std::vector<u8> to_hu;          // queued for the head unit
	std::vector<u8> junk;           // where the TLS server writes outside a call
	unsigned clock = 1000;

	// what the phone saw
	bool version_req = false, auth_ok = false, focus = false, bye_answered = false;
	u16 version_major = 0;
	std::vector<u8> discovery;
	std::map<u8, int> open_status;
	int config_status = -1, config_unacked = -1;
	int acks = 0, ack_session = -1;
	int pings_from_hu = 0, pong = 0;
	std::vector<u8> video_got;      // what the HU handed over
	int video_calls = 0, video_configs = 0;
	std::string phase_seen;
	bool client_cert_seen = false;
	std::map<u32, int> got;         // (channel << 16 | id) -> count, HU -> phone
	std::vector<int> sensor_statuses;
	int sensor_batches = 0, key_binding_status = -1, audio_focus_state = -1, nav_focus = -1;
	int mic_status = -1;

	// --pb: protobuf-encoded phone messages in, the HU's messages out
	std::string pbdir;
	FILE       *index = nullptr;
	int         seq = 0;

	// The phone's message `name`: from --pb DIR, else `own` (pb::Writer).
	std::vector<u8> body(const char *name, const std::vector<u8> &own)
	{
		if (pbdir.empty())
			return own;
		std::string b = slurp((pbdir + "/phone/" + name + ".bin").c_str());
		CHECK(!b.empty());
		return std::vector<u8>(b.begin(), b.end());
	}
	void dump(u8 ch, u8 flags, u16 id, const u8 *p, usize n)
	{
		if (index == nullptr)
			return;
		char name[32];
		std::snprintf(name, sizeof(name), "/hu/%04d.bin", seq);
		FILE *f = std::fopen((pbdir + name).c_str(), "wb");
		CHECK(f != nullptr);
		if (f) {
			std::fwrite(p, 1, n, f);
			std::fclose(f);
		}
		std::fprintf(index, "%d %u 0x%02x 0x%04x\n", seq++, ch, flags, id);
	}

	// Host
	void send(const u8 *p, usize n) override
	{
		dec.feed(p, n, [this](const aap::Frame &f) { on_frame(f); });
	}
	void phase(aa::Phase p, const std::string &) override { phase_seen += std::string(aa::phase_name(p)) + ","; }
	void video_frame(const u8 *p, usize n, bool config)
	{
		video_got.insert(video_got.end(), p, p + n);
		video_calls++;
		video_configs += config;
	}
	void video(const u8 *p, usize n, u64, bool config) override { video_frame(p, n, config); }
	unsigned now_ms() override { return clock; }

	void flush()
	{
		while (!to_hu.empty()) {
			std::vector<u8> b;
			b.swap(to_hu);
			// in odd-sized pieces, as USB reads would
			for (usize off = 0; off < b.size(); off += 1000)
				hu->rx(b.data() + off, std::min<usize>(1000, b.size() - off));
		}
	}

	// phone -> HU message, split into frames of at most `frag` plaintext bytes
	void msg(u8 ch, u8 flags, u16 id, const std::vector<u8> &body, usize frag = 0x4000)
	{
		std::vector<u8> plain = {(u8)(id >> 8), (u8)id};
		plain.insert(plain.end(), body.begin(), body.end());
		for (usize off = 0; off < plain.size();) {
			usize len = std::min(frag, plain.size() - off);
			u8 type = (off == 0 ? aap::kFirst : 0) | (off + len == plain.size() ? aap::kLast : 0);
			std::vector<u8> payload(plain.begin() + (isize)off, plain.begin() + (isize)(off + len));
			if (flags & aap::kEncrypted) {
				std::vector<u8> rec;
				tls.out = &rec;
				CHECK(mbedtls_ssl_write(&tls.ssl, payload.data(), payload.size()) == (int)payload.size());
				tls.out = &junk;
				payload = rec;
			}
			aap::put_frame(to_hu, ch, flags | type, (u32)plain.size(), payload.data(), payload.size());
			off += len;
		}
	}

	void on_frame(const aap::Frame &f)
	{
		std::vector<u8> plain(f.payload, f.payload + f.size);
		if (f.flags & aap::kEncrypted) {
			tls.in.insert(tls.in.end(), f.payload, f.payload + f.size);
			plain.clear();
			u8 buf[20000];
			int r;
			while ((r = mbedtls_ssl_read(&tls.ssl, buf, sizeof(buf))) > 0)
				plain.insert(plain.end(), buf, buf + r);
			CHECK(r == MBEDTLS_ERR_SSL_WANT_READ);
		}
		if (as.add(f.channel, f.flags, plain.data(), plain.size()) != aap::Assembler::Result::Done)
			return;
		const std::vector<u8> &m = as.message(f.channel);
		u16 id = (u16)(m[0] << 8 | m[1]);
		on_message(f.channel, f.flags, id, m.data() + 2, m.size() - 2);
	}

	void on_message(u8 ch, u8 flags, u16 id, const u8 *p, usize n)
	{
		u64 v = 0;
		got[(u32)ch << 16 | id]++;
		dump(ch, flags, id, p, n);
		if (ch == 0 && id == 1) {                        // VERSION_REQUEST
			version_req = true;
			version_major = (u16)(p[0] << 8 | p[1]);
			msg(0, 0, 2, {0, 1, 0, 7, 0, 0});            // 1.7, status 0
		} else if (ch == 0 && id == 3) {                 // TLS
			tls.in.insert(tls.in.end(), p, p + n);
			std::vector<u8> out;
			tls.out = &out;
			int r = mbedtls_ssl_handshake(&tls.ssl);
			tls.out = &junk;
			CHECK(r == 0 || r == MBEDTLS_ERR_SSL_WANT_READ);
			if (r == 0) {
				tls.done = true;
				client_cert_seen = mbedtls_ssl_get_peer_cert(&tls.ssl) != nullptr;
			}
			if (!out.empty())
				msg(0, 0, 3, out);
		} else if (ch == 0 && id == 4) {                 // AUTH_COMPLETE
			CHECK(tls.done);
			auth_ok = pb::get_u(p, n, 1, v) && v == 0;
			pb::Writer w;
			w.s(4, "Android Auto").s(5, "Test Phone");
			msg(0, aap::kEncrypted, 5, body("sd_request", w.buf));
		} else if (ch == 0 && id == 6) {                 // SERVICE_DISCOVERY_RESPONSE
			CHECK(flags & aap::kEncrypted);
			discovery.assign(p, p + n);
		} else if (ch == 0 && id == 11) {                // PING_REQUEST
			pings_from_hu++;
		} else if (ch == 0 && id == 12) {
			pong++;
		} else if (ch == 0 && id == 16) {
			bye_answered = true;
		} else if (id == 8 && (flags & aap::kControl)) { // CHANNEL_OPEN_RESPONSE
			CHECK(pb::get_u(p, n, 1, v));
			open_status[ch] = (int)(i32)v;
		} else if (ch == 3 && id == 0x8003) {            // CONFIG
			pb::get_u(p, n, 1, v);
			config_status = (int)v;
			pb::get_u(p, n, 2, v);
			config_unacked = (int)v;
		} else if (ch == 3 && id == 0x8008) {            // VIDEO_FOCUS_NOTIFICATION
			focus = pb::get_u(p, n, 1, v) && v == 1;
		} else if (ch == 3 && id == 0x8004) {            // ACK
			acks++;
			pb::get_u(p, n, 1, v);
			ack_session = (int)v;
		} else if (ch == 0 && id == 19) {                // AUDIO_FOCUS_NOTIFICATION
			pb::get_u(p, n, 1, v);
			audio_focus_state = (int)v;
		} else if (ch == 7 && id == 0x8006) {            // MICROPHONE_RESPONSE
			CHECK(pb::get_u(p, n, 1, v));
			mic_status = (int)(i32)v;
		} else if (ch == 0 && id == 14) {                // NAV_FOCUS_NOTIFICATION
			pb::get_u(p, n, 1, v);
			nav_focus = (int)v;
		} else if (ch == 2 && id == 0x8002) {            // SENSOR_RESPONSE, in request order
			pb::get_u(p, n, 1, v);
			sensor_statuses.push_back((int)(i32)v);
		} else if (ch == 2 && id == 0x8003) {            // SENSOR_BATCH
			sensor_batches++;
		} else if (ch == 1 && id == 0x8003) {            // KEY_BINDING_RESPONSE
			pb::get_u(p, n, 1, v);
			key_binding_status = (int)(i32)v;
		}
	}
};

// The video service in the discovery response: H.264 BP 800x480.
void check_discovery(const std::vector<u8> &d)
{
	pb::Reader r(d.data(), d.size());
	pb::Field f;
	int services = 0;
	bool video = false;
	while (r.next(f)) {
		if (f.num != 1)
			continue;
		services++;
		u64 id = 0, codec = 0, res = 0;
		const u8 *sink, *conf;
		usize sn, cn;
		pb::get_u(f.p, f.n, 1, id);
		if (id == aa::kChVideo && pb::get_bytes(f.p, f.n, 3, sink, sn) &&
		    pb::get_u(sink, sn, 1, codec) && pb::get_bytes(sink, sn, 4, conf, cn) &&
		    pb::get_u(conf, cn, 1, res))
			video = codec == 3 && res == 1;
	}
	CHECK(r.ok());
	CHECK_EQ(services, 7);                 // video, 3 audio, mic, sensors, input
	CHECK(video);
	CHECK(pb::get_str(d.data(), d.size(), 14) == "MMI 3G+");
}

void test_session(const std::string &crt, const std::string &key, const std::string &pbdir)
{
	Phone phone;
	phone.pbdir = pbdir;
	if (!pbdir.empty()) {
		phone.index = std::fopen((pbdir + "/hu/index.txt").c_str(), "w");
		CHECK(phone.index != nullptr);
	}
	CHECK(phone.tls.init(crt, key));
	phone.tls.out = &phone.junk;
	aa::Config cfg;
	aa::Session hu(cfg, phone);
	phone.hu = &hu;

	CHECK(hu.start(crt, key));
	CHECK(phone.version_req);
	CHECK_EQ(phone.version_major, 1);
	// version, TLS, auth, discovery: a few rounds
	for (int i = 0; i < 10 && hu.phase() != aa::Phase::Running; i++)
		phone.flush();
	CHECK(hu.phase() == aa::Phase::Running);
	CHECK(phone.tls.done);
	CHECK(phone.client_cert_seen);
	CHECK(phone.auth_ok);
	CHECK(hu.stats().phone == "Test Phone");
	check_discovery(phone.discovery);
	CHECK(phone.discovery == hu.discovery_response());

	// channels: video ok, an unknown one refused
	pb::Writer open;
	open.si(1, -1).i(2, 3);                          // priority is sint32
	std::vector<u8> open_req = phone.body("channel_open", open.buf);
	phone.msg(3, aap::kEncrypted | aap::kControl, 7, open_req);
	phone.msg(9, aap::kEncrypted | aap::kControl, 7, open_req);
	for (u8 ch : {aa::kChInput, aa::kChSensor, aa::kChMediaAudio})
		phone.msg(ch, aap::kEncrypted | aap::kControl, 7, open_req);
	phone.flush();
	CHECK_EQ(phone.open_status[3], 0);
	CHECK_EQ(phone.open_status[9], -5);
	CHECK_EQ(phone.open_status[aa::kChInput], 0);
	CHECK_EQ(phone.open_status[aa::kChSensor], 0);
	CHECK_EQ(phone.open_status[aa::kChMediaAudio], 0);
	CHECK_EQ(hu.stats().open_channels, 1u << 1 | 1u << 2 | 1u << 3 | 1u << 4);

	// setup -> config ready + focus; start; codec config; a 40 KB frame in
	// 16 KB fragments; a small one
	pb::Writer setup, start;
	setup.u(1, 3);
	phone.msg(3, aap::kEncrypted, 0x8000, phone.body("setup", setup.buf));
	start.i(1, 7).u(2, 0);
	phone.msg(3, aap::kEncrypted, 0x8001, phone.body("start", start.buf));
	phone.msg(3, aap::kEncrypted, 0x0001, {0, 0, 0, 1, 0x67, 0x42});
	std::vector<u8> big(8 + 40000);
	for (usize i = 8; i < big.size(); i++)
		big[i] = (u8)i;
	big[6] = 0x03;                                   // timestamp 1000 us
	big[7] = 0xe8;
	phone.msg(3, aap::kEncrypted, 0x0000, big);
	// the second frame comes at once (timestamp 9 us: 1 ms older than the
	// first's, so 1 ms "older" on arrival): ack pacing holds its ack for a
	// frame period (25 fps: 40 ms)
	phone.msg(3, aap::kEncrypted, 0x0000, {0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 1, 0x41});
	phone.flush();
	CHECK_EQ(phone.config_status, 2);
	CHECK_EQ(phone.config_unacked, 1);
	CHECK(phone.focus);
	CHECK_EQ(phone.acks, 2);                         // codec config + first frame
	CHECK_EQ(hu.ms_to_next(50), 40);
	CHECK_EQ(hu.ms_to_next(10), 10);
	phone.clock += 39;
	hu.tick();
	CHECK_EQ(phone.acks, 2);
	CHECK_EQ(hu.ms_to_next(50), 1);
	phone.clock += 1;
	hu.tick();
	CHECK_EQ(phone.acks, 3);
	CHECK_EQ(hu.ms_to_next(50), 50);
	CHECK_EQ(hu.stats().acks_held, 1);
	CHECK_EQ(hu.stats().ack_wait_ms_max, 40);
	CHECK_EQ(hu.stats().age_ms, 1);
	// a frame 100 ms later: acked at once (no burst saved up, no wait)
	phone.clock += 100;
	phone.msg(3, aap::kEncrypted, 0x0000, {0, 0, 0, 0, 0, 0, 0, 9, 0, 0, 0, 1, 0x41});
	phone.flush();
	CHECK_EQ(phone.acks, 4);
	CHECK_EQ(hu.stats().acks_held, 1);
	CHECK_EQ(hu.stats().age_ms, 141);                // its timestamp 9 us again, 140 ms later
	CHECK_EQ(hu.stats().age_ms_max, 141);
	CHECK_EQ(phone.ack_session, 7);
	CHECK_EQ(phone.video_calls, 4);
	CHECK_EQ(phone.video_configs, 1);
	CHECK_EQ(phone.video_got.size(), 6 + 40000 + 5 + 5);
	CHECK_EQ(phone.video_got[6 + 1], 9);                 // byte 9 of `big`
	CHECK_EQ(hu.stats().video_frames, 3);

	// focus: audio and navigation granted, video re-sent on request
	pb::Writer af, nf, vf;
	af.u(1, 1);
	nf.u(1, 2);
	vf.u(2, 1).u(3, 0);
	phone.focus = false;
	phone.msg(0, aap::kEncrypted, 18, phone.body("audio_focus", af.buf));
	phone.msg(0, aap::kEncrypted, 13, phone.body("nav_focus", nf.buf));
	phone.msg(3, aap::kEncrypted, 0x8007, phone.body("video_focus", vf.buf));
	phone.flush();
	CHECK_EQ(phone.audio_focus_state, 1);
	CHECK_EQ(phone.nav_focus, 2);
	CHECK(phone.focus);

	// microphone: opened (no data follows), answered OK
	pb::Writer mr;
	mr.b(1, true);
	phone.msg(7, aap::kEncrypted, 0x8005, phone.body("mic_request", mr.buf));
	phone.flush();
	CHECK_EQ(phone.mic_status, 0);

	// sensors: driving status and night mode answered with a batch each,
	// speed refused; key binding accepted
	pb::Writer s1, s2, s3, kb;
	s1.u(1, 13).i(2, 0);
	s2.u(1, 10).i(2, 1000);
	s3.u(1, 3).i(2, 100);
	kb.packed(1, {19, 20, 23, 65536});
	phone.msg(2, aap::kEncrypted, 0x8001, phone.body("sensor_driving", s1.buf));
	phone.msg(2, aap::kEncrypted, 0x8001, phone.body("sensor_night", s2.buf));
	phone.msg(2, aap::kEncrypted, 0x8001, phone.body("sensor_speed", s3.buf));
	phone.msg(1, aap::kEncrypted, 0x8002, phone.body("key_binding", kb.buf));
	phone.flush();
	CHECK(phone.sensor_statuses == std::vector<int>({0, 0, -9}));
	CHECK_EQ(phone.sensor_batches, 2);
	CHECK_EQ(phone.key_binding_status, 0);
	CHECK_EQ(hu.stats().unknown, 0);

	// pings: ours after ping_ms, the phone's answered
	phone.clock += 1500;
	hu.tick();
	CHECK_EQ(phone.pings_from_hu, 1);
	pb::Writer ping, pong;
	ping.i(1, 123);
	phone.msg(0, 0, 11, phone.body("ping", ping.buf));
	pong.i(1, 2140);                                 // our ping's timestamp back
	phone.msg(0, 0, 12, phone.body("pong", pong.buf));
	phone.flush();
	CHECK_EQ(phone.pong, 1);
	CHECK_EQ(hu.stats().pongs, 1);
	CHECK_EQ(hu.stats().last_rtt_ms, 500);

	// bye-bye from the phone
	pb::Writer bye;
	bye.u(1, 1);
	phone.msg(0, aap::kEncrypted, 15, phone.body("byebye", bye.buf));
	phone.flush();
	CHECK(phone.bye_answered);
	CHECK(hu.phase() == aa::Phase::Ended);
	CHECK(phone.phase_seen == "version,TLS handshake,service discovery,running,ended,");
	if (phone.index)
		std::fclose(phone.index);
}

void test_bad_cert()
{
	struct Null : aa::Host {
		void send(const u8 *, usize) override {}
		unsigned now_ms() override { return 0; }
	} host;
	aa::Session hu(aa::Config{}, host);
	CHECK(!hu.start("not a cert", "not a key"));
	CHECK(hu.phase() == aa::Phase::Failed);
	CHECK(hu.stats().error.find("certificate") != std::string::npos);
}

} // namespace

int main(int argc, char **argv)
{
	const char *prog = argv[0];
	std::string pbdir;
	if (argc > 2 && std::string(argv[1]) == "--pb") {
		pbdir = argv[2];
		argc -= 2;
		argv += 2;
	}
	const char *crt = argc > 2 ? argv[1] : "third_party/aasdk-cert/headunit.crt";
	const char *key = argc > 2 ? argv[2] : "third_party/aasdk-cert/headunit.key";
	std::string c = slurp(crt), k = slurp(key);
	if (c.empty() || k.empty()) {
		std::fprintf(stderr, "%s: no head unit certificate (%s, %s)\n", prog, crt, key);
		return 2;
	}
	test_pb();
	test_frames();
	test_session(c, k, pbdir);
	test_bad_cert();
	CHECK_DONE();
}
