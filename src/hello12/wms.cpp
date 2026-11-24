// wms.cpp - see wms.h.
#include "wms.h"

#include <cstring>

namespace wms {

namespace {

void put_i32(u8 *p, i32 v)
{
	u32 u = (u32)v;
	p[0] = (u8)u;
	p[1] = (u8)(u >> 8);
	p[2] = (u8)(u >> 16);
	p[3] = (u8)(u >> 24);
}

} // namespace

Answer devctl(u32 dcmd, u32 nbytes, i32 codec, i32 rate)
{
	Answer a;
	switch (dcmd) {
	case kDcmdConnected:
		a.handled = true;
		a.name = "connected?";
		put_i32(a.out, 1);
		a.len = 4;
		break;
	case kDcmdStreamInfo:
		a.handled = true;
		a.name = "stream info";
		put_i32(a.out, 1);       // streaming
		put_i32(a.out + 4, codec);
		put_i32(a.out + 8, rate);
		put_i32(a.out + 12, 0);
		a.len = 16;
		break;
	case kDcmdEof:
		a.handled = true;
		a.name = "end of stream";
		break;
	default:
		break;
	}
	if (a.len > nbytes)
		a.len = nbytes;
	return a;
}

void copy_looped(const u8 *data, usize size, u64 off, u8 *out, usize n)
{
	if (size == 0) {
		std::memset(out, 0, n);
		return;
	}
	usize at = (usize)(off % size);
	while (n > 0) {
		usize k = size - at < n ? size - at : n;
		std::memcpy(out, data + at, k);
		out += k;
		n -= k;
		at = 0;
	}
}

} // namespace wms
