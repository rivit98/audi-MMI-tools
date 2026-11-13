// usb_chunk.h - how much of an outgoing byte stream the next USB bulk OUT
// transfer carries.
//
// A transfer whose length is a multiple of the endpoint's packet size ends
// without a short packet, and an Android accessory read on the phone keeps
// waiting for more. Instead of a zero-length packet (never tried on
// io-usb), such a block goes out as two transfers: all but its last byte,
// then the last byte. For a stream protocol (Android Auto's frames) where
// the transfers fall makes no difference.
#ifndef COMMON_USB_CHUNK_H
#define COMMON_USB_CHUNK_H

#include "types.h"
#include <cstddef>

// The length of the next transfer out of `left` bytes; max_xfer = the
// transfer buffer, packet = wMaxPacketSize (0: unknown).
inline usize usb_chunk(usize left, usize max_xfer, unsigned packet)
{
	usize n = left < max_xfer ? left : max_xfer;
	if (packet != 0 && n > 1 && n % packet == 0)
		n--;
	return n;
}

#endif
