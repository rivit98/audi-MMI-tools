// pb.h - a minimal protobuf (proto2 wire format) writer and reader, our
// own, header-only: a Writer that builds a message field by field and a
// Reader that walks the fields of one. No schema, no code generator: the
// user writes each message with its field numbers next to it (android_auto:
// session.cpp, after aasdk's aap_protobuf/*.proto).
//
// Wire types: 0 varint, 1 64-bit, 2 length-delimited, 5 32-bit. A
// negative int32 is a 10-byte varint (sign-extended to 64 bits); sint32 /
// sint64 are zigzag (si(), unzigzag()).
#ifndef COMMON_PB_H
#define COMMON_PB_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pb {

class Writer {
public:
	std::vector<uint8_t> buf;

	void varint(uint64_t v)
	{
		while (v >= 0x80) {
			buf.push_back((uint8_t)(v | 0x80));
			v >>= 7;
		}
		buf.push_back((uint8_t)v);
	}
	void tag(unsigned field, unsigned wire) { varint((uint64_t)field << 3 | wire); }

	// uint32 / uint64 / enum >= 0
	Writer &u(unsigned field, uint64_t v)
	{
		tag(field, 0);
		varint(v);
		return *this;
	}
	// int32 / int64 / enum, negative included
	Writer &i(unsigned field, int64_t v) { return u(field, (uint64_t)v); }
	// sint32 / sint64 (zigzag)
	Writer &si(unsigned field, int64_t v) { return u(field, ((uint64_t)v << 1) ^ (uint64_t)(v >> 63)); }
	Writer &b(unsigned field, bool v) { return u(field, v ? 1 : 0); }
	Writer &bytes(unsigned field, const void *p, size_t n)
	{
		tag(field, 2);
		varint(n);
		const uint8_t *s = static_cast<const uint8_t *>(p);
		buf.insert(buf.end(), s, s + n);
		return *this;
	}
	Writer &s(unsigned field, std::string_view v) { return bytes(field, v.data(), v.size()); }
	Writer &msg(unsigned field, const Writer &sub) { return bytes(field, sub.buf.data(), sub.buf.size()); }
	// repeated int32 [packed = true]
	Writer &packed(unsigned field, const std::vector<int32_t> &v)
	{
		Writer w;
		for (int32_t x : v)
			w.varint((uint64_t)(int64_t)x);
		return msg(field, w);
	}
};

struct Field {
	unsigned       num = 0;
	unsigned       wire = 0;
	uint64_t       v = 0;           // wire 0, 1, 5
	const uint8_t *p = nullptr;     // wire 2
	size_t         n = 0;
};

class Reader {
public:
	Reader(const uint8_t *p, size_t n) : p_(p), end_(p + n) {}

	// The next field; false at the end or on malformed input (then ok()
	// is false).
	bool next(Field &f)
	{
		if (p_ == end_ || bad_)
			return false;
		uint64_t key;
		if (!read_varint(key))
			return fail();
		f.num = (unsigned)(key >> 3);
		f.wire = (unsigned)(key & 7);
		f.p = nullptr;
		f.n = 0;
		switch (f.wire) {
		case 0:
			if (!read_varint(f.v))
				return fail();
			return true;
		case 1:
			return fixed(f, 8);
		case 5:
			return fixed(f, 4);
		case 2: {
			uint64_t n;
			if (!read_varint(n) || n > (uint64_t)(end_ - p_))
				return fail();
			f.p = p_;
			f.n = (size_t)n;
			p_ += n;
			return true;
		}
		}
		return fail();
	}
	bool ok() const { return !bad_; }

private:
	bool fail()
	{
		bad_ = true;
		return false;
	}
	bool read_varint(uint64_t &v)
	{
		v = 0;
		for (int shift = 0; shift < 64 && p_ < end_; shift += 7) {
			uint8_t c = *p_++;
			v |= (uint64_t)(c & 0x7f) << shift;
			if (!(c & 0x80))
				return true;
		}
		return false;
	}
	bool fixed(Field &f, int n)
	{
		if (end_ - p_ < n)
			return fail();
		f.v = 0;
		for (int k = 0; k < n; k++)
			f.v |= (uint64_t)p_[k] << (8 * k);
		p_ += n;
		return true;
	}

	const uint8_t *p_, *end_;
	bool           bad_ = false;
};

// A sint32 / sint64 varint's value (zigzag).
inline int64_t unzigzag(uint64_t v)
{
	return (int64_t)(v >> 1) ^ -(int64_t)(v & 1);
}

// The last varint field `num` of a message (proto2: the last one wins).
inline bool get_u(const uint8_t *p, size_t n, unsigned num, uint64_t &out)
{
	Reader r(p, n);
	Field f;
	bool found = false;
	while (r.next(f))
		if (f.num == num && f.wire == 0) {
			out = f.v;
			found = true;
		}
	return found && r.ok();
}

// The last length-delimited field `num` (string, bytes, sub-message).
inline bool get_bytes(const uint8_t *p, size_t n, unsigned num, const uint8_t *&out, size_t &out_n)
{
	Reader r(p, n);
	Field f;
	bool found = false;
	while (r.next(f))
		if (f.num == num && f.wire == 2) {
			out = f.p;
			out_n = f.n;
			found = true;
		}
	return found && r.ok();
}

inline std::string_view get_str(const uint8_t *p, size_t n, unsigned num)
{
	const uint8_t *s;
	size_t sn;
	if (!get_bytes(p, n, num, s, sn))
		return {};
	return std::string_view(reinterpret_cast<const char *>(s), sn);
}

} // namespace pb

#endif
