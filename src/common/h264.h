// h264.h - the H.264 bits a live stream to the unit's DM642 needs (pure C++,
// no QNX calls: tests/test_live.cpp runs it on the build host).
//
//   - Annex-B byte streams split into NAL units and access units (frames)
//   - SPS / PPS / slice header fields: picture size, frame_num, POC
//   - a "skip frame": a P picture whose macroblocks are all skipped, so it
//     repeats the previous picture. It is not a reference picture, so the
//     encoder's own frames after it stay valid as they are. A live stream
//     inserts them where the sender left a gap (live_stream.h): io-media
//     keeps ~9 frames queued and only shows them when more data comes
//     (docs/phone-mirroring.md 7.10).
//
// Only what Baseline (and Main without fields) uses is supported: CAVLC,
// frames (no fields), one slice group, no weighted prediction.
#ifndef COMMON_H264_H
#define COMMON_H264_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace h264 {

enum NalType : uint8_t {
	kSlice = 1,
	kIdr = 5,
	kSei = 6,
	kSps = 7,
	kPps = 8,
	kAud = 9,
	kFiller = 12,
};

// One NAL unit inside a buffer: header byte first, no start code.
struct Nal {
	const uint8_t *p;
	size_t n;
	uint8_t type() const { return p[0] & 0x1f; }
	uint8_t ref_idc() const { return (uint8_t)(p[0] >> 5 & 3); }
};

// The NAL units of an Annex-B stream (00 00 01 / 00 00 00 01 start codes).
// Trailing zero bytes of each unit are dropped. Empty units are skipped.
std::vector<Nal> split_annexb(const uint8_t *p, size_t n);

// Access units (frames) of an Annex-B stream, as [begin, end) byte ranges
// of the input, start codes included. A new unit starts at an AUD, SPS,
// PPS or SEI after a slice, or at a slice with first_mb_in_slice == 0 after
// a slice (media/csmp4.py's rule).
struct Range {
	size_t begin, end;
};
std::vector<Range> split_access_units(const uint8_t *p, size_t n);

struct Sps {
	uint8_t profile = 0, constraints = 0, level = 0;
	unsigned id = 0;
	unsigned chroma_format = 1;
	unsigned log2_max_frame_num = 4;
	unsigned poc_type = 0;
	unsigned log2_max_poc_lsb = 4;          // poc_type 0
	bool delta_pic_order_always_zero = false;  // poc_type 1
	unsigned max_ref_frames = 0;
	unsigned width_mbs = 0, height_mbs = 0;    // frames only
	bool frame_mbs_only = true;
	unsigned width = 0, height = 0;            // after cropping
	unsigned mbs() const { return width_mbs * height_mbs; }
};

struct Pps {
	unsigned id = 0, sps_id = 0;
	bool cabac = false;
	bool bottom_field_pic_order_present = false;
	unsigned slice_groups = 1;
	unsigned num_ref_idx_l0_default = 1;
	bool weighted_pred = false;
	bool deblocking_control_present = false;
	bool redundant_pic_cnt_present = false;
};

// "" or what is wrong / unsupported. nal: the whole NAL unit, header first.
std::string parse_sps(const uint8_t *nal, size_t n, Sps &out);
std::string parse_pps(const uint8_t *nal, size_t n, Pps &out);

struct Slice {
	uint8_t nal_type = 0, ref_idc = 0;
	unsigned first_mb = 0, slice_type = 0, pps_id = 0;
	unsigned frame_num = 0;
	unsigned idr_pic_id = 0;
	unsigned poc_lsb = 0;                      // poc_type 0
	bool idr() const { return nal_type == kIdr; }
};

// The start of a slice header (up to the POC fields).
std::string parse_slice(const uint8_t *nal, size_t n, const Sps &sps, const Pps &pps, Slice &out);

// Can skip frames be made for this SPS/PPS? "" or why not.
std::string skip_supported(const Sps &sps, const Pps &pps);

// A non-reference P slice covering the whole picture with every macroblock
// skipped (NAL unit, header first, emulation prevention applied). frame_num
// must be the previous reference picture's frame_num + 1 (mod max); poc_lsb
// is used for poc_type 0 only.
std::vector<uint8_t> skip_slice(const Sps &sps, const Pps &pps, unsigned frame_num,
                                unsigned poc_lsb);

// RBSP -> NAL payload: insert 0x03 after two zero bytes before 00..03.
void escape(const std::vector<uint8_t> &rbsp, std::vector<uint8_t> &out);

} // namespace h264

#endif
