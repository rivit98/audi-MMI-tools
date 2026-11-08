// csmp4.h - the constant-sample-size MP4 ("CS-MP4") a live H.264 stream to
// io-media is served as (pure C++; tests/test_live.cpp runs it on the build
// host). Plays on the car (docs/phone-mirroring.md 7.10).
//
//   ftyp | moov (one-entry stts/stsc/stco, stsz with one constant size S) |
//   mdat with an 8-byte header | frame 0 | frame 1 | ...
//
// Every frame is S bytes: its NAL units with 4-byte lengths, padded with a
// filler NAL. Frame k sits at data_off + k * S, so the header can be
// written before any frame exists and declare far more frames than will
// come. io-media's own MP4 demuxer reads 32-bit box sizes only, so mdat has
// an 8-byte header (size 0 = to the end of the file when over-declared)
// and the whole stream stays under 4 GB (docs/phone-mirroring.md 7.9).
//
// build_header() writes what media/csmp4.py writes, byte for byte (checked
// by make test).
#ifndef COMMON_CSMP4_H
#define COMMON_CSMP4_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace csmp4 {

// A filler NAL with its length prefix is at least this long.
constexpr uint32_t kFillerMin = 4 + 2;

struct Info {
	uint64_t data_off = 0;      // frame 0's first byte (stco[0])
	uint32_t sample_size = 0;   // S, stsz's constant size
	uint32_t declared = 0;      // frames the header declares (stsz count)
	uint32_t present = 0;       // whole frames in the file (parse() only)
	uint32_t timescale = 0;     // mdhd
	uint32_t delta = 0;         // stts' one sample duration

	uint64_t total() const { return data_off + (uint64_t)declared * sample_size; }
	double fps() const { return delta ? (double)timescale / delta : 0; }
};

// Parse the start of a CS-MP4 (head[0..len), at least ftyp + moov + the
// mdat header) of a file file_size bytes long. "" on success, else what is
// wrong, including everything io-media would refuse.
std::string parse(const uint8_t *head, size_t len, uint64_t file_size, Info &out);

// The most frames a stream of S-byte frames may declare (under 4 GB with a
// header of up to hdr_max bytes).
uint32_t max_frames(uint32_t sample_size, uint32_t hdr_max = 4096);

// ftyp + moov + the mdat header for a stream of declared frames of S bytes
// at fps (whole frames per second), from the SPS / PPS NAL units (header
// byte first, no start code). mdat size 0 ("to the end of the file"),
// unless exact_mdat. "" or what is wrong; out = the header.
std::string build_header(const std::vector<uint8_t> &sps, const std::vector<uint8_t> &pps,
                         unsigned fps, uint32_t sample_size, uint32_t declared,
                         std::vector<uint8_t> &out, bool exact_mdat = false);

// One frame as a sample: its NAL units (Annex-B in, AUDs dropped) with
// 4-byte big-endian lengths, then a filler NAL up to S bytes. "" or why
// not (too big for S, no NAL units). out is resized to S.
std::string make_sample(const uint8_t *annexb, size_t n, uint32_t sample_size,
                        std::vector<uint8_t> &out);
// The same from NAL units already split (h264::Nal-like: header first).
std::string make_sample_nals(const std::vector<std::pair<const uint8_t *, size_t>> &nals,
                             uint32_t sample_size, std::vector<uint8_t> &out);

// A sample that is only a filler NAL (what a far read past the live edge
// gets: never decoded, just bytes).
void filler_sample(uint32_t sample_size, std::vector<uint8_t> &out);

} // namespace csmp4

#endif
