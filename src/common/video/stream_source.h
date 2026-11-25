// stream_source.h - what StreamServer (stream_server.h) serves: a file that
// grows while it is read. A header of data_off() bytes, then slots of
// sample_size() bytes (a frame, or a chunk of frames) that become available
// one after another, up to total(). live::Stream (live_stream.h) is one; a
// pre-built file released on a clock (hello9's PacedFile) is another.
#ifndef COMMON_STREAM_SOURCE_H
#define COMMON_STREAM_SOURCE_H

#include "types.h"

class StreamSource {
public:
	// What one read() served as filler instead of data.
	struct ReadInfo {
		unsigned evicted = 0;   // slots no longer kept
		unsigned ahead = 0;     // slots not there yet (a far read)
	};

	virtual ~StreamSource() = default;

	virtual u64 data_off() const = 0;
	virtual u32 sample_size() const = 0;
	virtual u32 declared() const = 0;       // slots the header declares
	virtual u64 total() const = 0;
	virtual unsigned slots_per_s() const = 0;
	virtual u64 frames() const = 0;         // slots there now
	virtual u64 available() const = 0;      // bytes [0, available()) exist now
	// Copy [off, off + n) into dst, n clamped at total(); returns the count.
	virtual usize read(u64 off, u8 *dst, usize n, ReadInfo *info) = 0;
	// Called by the server's thread every ~10 ms; returns slots added.
	virtual unsigned fill(u64 now_ms) = 0;
	virtual void finish() = 0;              // reads past the end get EOF from now on
	virtual bool finished() const = 0;
};

#endif
