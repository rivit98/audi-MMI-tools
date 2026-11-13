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
#include "types.h"
#include <string_view>
#include <vector>

namespace pb {

class Writer {
public:
	std::vector<u8> buf;

	void varint(u64 v)
	{
		while (v >= 0x80) {
			buf.push_back((u8)(v | 0x80));
			v >>= 7;
		}
		buf.push_back((u8)v);
	}
	void tag(unsigned field, unsigned wire) { varint((u64)field << 3 | wire); }

	// uint32 / uint64 / enum >= 0
	Writer &u(unsigned field, u64 v)
	{
		tag(field, 0);
		varint(v);
		return *this;
	}
	// int32 / int64 / enum, negative included
	Writer &i(unsigned field, i64 v) { return u(field, (u64)v); }
	// sint32 / sint64 (zigzag)
	Writer &si(unsigned field, i64 v) { return u(field, ((u64)v << 1) ^ (u64)(v >> 63)); }
	Writer &b(unsigned field, bool v) { return u(field, v ? 1 : 0); }
	Writer &bytes(unsigned field, const void *p, usize n)
	{
		tag(field, 2);
		varint(n);
		const u8 *s = static_cast<const u8 *>(p);
		buf.insert(buf.end(), s, s + n);
		return *this;
	}
	Writer &s(unsigned field, std::string_view v) { return bytes(field, v.data(), v.size()); }
	Writer &msg(unsigned field, const Writer &sub) { return bytes(field, sub.buf.data(), sub.buf.size()); }
	// repeated int32 [packed = true]
	Writer &packed(unsigned field, const std::vector<i32> &v)
	{
		Writer w;
		for (i32 x : v)
			w.varint((u64)(i64)x);
		return msg(field, w);
	}
};

struct Field {
	unsigned       num = 0;
	unsigned       wire = 0;
	u64       v = 0;           // wire 0, 1, 5
	const u8 *p = nullptr;     // wire 2
	usize         n = 0;
};

class Reader {
public:
	Reader(const u8 *p, usize n) : p_(p), end_(p + n) {}

	// The next field; false at the end or on malformed input (then ok()
	// is false).
	bool next(Field &f)
	{
		if (p_ == end_ || bad_)
			return false;
		u64 key;
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
			u64 n;
			if (!read_varint(n) || n > (u64)(end_ - p_))
				return fail();
			f.p = p_;
			f.n = (usize)n;
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
	bool read_varint(u64 &v)
	{
		v = 0;
		for (int shift = 0; shift < 64 && p_ < end_; shift += 7) {
			u8 c = *p_++;
			v |= (u64)(c & 0x7f) << shift;
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
			f.v |= (u64)p_[k] << (8 * k);
		p_ += n;
		return true;
	}

	const u8 *p_, *end_;
	bool           bad_ = false;
};

// A sint32 / sint64 varint's value (zigzag).
inline i64 unzigzag(u64 v)
{
	return (i64)(v >> 1) ^ -(i64)(v & 1);
}

// The last varint field `num` of a message (proto2: the last one wins).
inline bool get_u(const u8 *p, usize n, unsigned num, u64 &out)
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
inline bool get_bytes(const u8 *p, usize n, unsigned num, const u8 *&out, usize &out_n)
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

inline std::string_view get_str(const u8 *p, usize n, unsigned num)
{
	const u8 *s;
	usize sn;
	if (!get_bytes(p, n, num, s, sn))
		return {};
	return std::string_view(reinterpret_cast<const char *>(s), sn);
}

} // namespace pb

#endif
