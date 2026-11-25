// h264.cpp - see h264.h. Syntax as in ITU-T H.264 7.3 (SPS 7.3.2.1.1,
// PPS 7.3.2.2, slice header 7.3.3, slice data 7.3.4).
#include "h264.h"
#include "types.h"

namespace h264 {

namespace {

// Reads bits of an RBSP, skipping emulation prevention bytes on the way.
class Bits {
public:
	Bits(const u8 *p, usize n) : p_(p), n_(n) {}

	bool fail() const { return fail_; }
	// Bits read so far, counted in the RBSP (emulation prevention bytes
	// not counted).
	usize pos() const { return rbsp_bytes_ * 8 - left_; }

	unsigned u(unsigned bits)
	{
		unsigned v = 0;
		while (bits-- > 0)
			v = v << 1 | bit();
		return v;
	}

	unsigned ue()
	{
		unsigned zeros = 0;
		while (!fail_ && bit() == 0)
			if (++zeros > 31) {
				fail_ = true;
				return 0;
			}
		if (zeros == 0)
			return 0;
		return (1u << zeros) - 1 + u(zeros);
	}

	int se()
	{
		unsigned k = ue();
		return k & 1 ? (int)((k + 1) / 2) : -(int)(k / 2);
	}

private:
	unsigned bit()
	{
		if (left_ == 0) {
			if (pos_ >= n_) {
				fail_ = true;
				return 0;
			}
			u8 b = p_[pos_++];
			// 00 00 03: the 03 is not data
			if (b == 3 && zeros_ >= 2) {
				zeros_ = 0;
				if (pos_ >= n_) {
					fail_ = true;
					return 0;
				}
				b = p_[pos_++];
			}
			zeros_ = b == 0 ? zeros_ + 1 : 0;
			cur_ = b;
			left_ = 8;
			rbsp_bytes_++;
		}
		left_--;
		return cur_ >> left_ & 1;
	}

	const u8 *p_;
	usize n_, pos_ = 0, rbsp_bytes_ = 0;
	unsigned zeros_ = 0, left_ = 0;
	u8 cur_ = 0;
	bool fail_ = false;
};

class Writer {
public:
	void u(unsigned bits, unsigned v)
	{
		while (bits-- > 0)
			bit(v >> bits & 1);
	}

	void ue(unsigned v)
	{
		unsigned x = v + 1, len = 0;
		for (unsigned t = x; t > 1; t >>= 1)
			len++;
		u(len, 0);
		u(len + 1, x);
	}

	void se(int v) { ue(v > 0 ? (unsigned)(2 * v - 1) : (unsigned)(-2 * v)); }

	// rbsp_trailing_bits: a 1, then zeros to the byte boundary
	void trailing()
	{
		bit(1);
		while (n_ != 0)
			bit(0);
	}

	std::vector<u8> out;

private:
	void bit(unsigned b)
	{
		cur_ = (u8)(cur_ << 1 | b);
		if (++n_ == 8) {
			out.push_back(cur_);
			cur_ = 0;
			n_ = 0;
		}
	}
	u8 cur_ = 0;
	unsigned n_ = 0;
};

bool is_slice(u8 t)
{
	return t == kSlice || t == kIdr;
}

} // namespace

std::vector<Nal> split_annexb(const u8 *p, usize n)
{
	std::vector<Nal> out;
	std::vector<usize> starts;

	for (usize i = 0; i + 2 < n;) {
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) {
			starts.push_back(i + 3);
			i += 3;
		} else {
			i++;
		}
	}
	for (usize k = 0; k < starts.size(); k++) {
		usize s = starts[k];
		usize e = k + 1 < starts.size() ? starts[k + 1] - 3 : n;
		while (e > s && p[e - 1] == 0)
			e--;
		if (e > s)
			out.push_back({p + s, e - s});
	}
	return out;
}

std::vector<Range> split_access_units(const u8 *p, usize n)
{
	std::vector<Range> out;
	std::vector<Nal> nals = split_annexb(p, n);
	usize begin = 0;
	bool have_slice = false;

	for (const Nal &nal : nals) {
		u8 t = nal.type();
		bool starts_new = false;
		if (have_slice && (t == kAud || t == kSps || t == kPps || t == kSei))
			starts_new = true;
		else if (have_slice && is_slice(t) && nal.n > 1 && (nal.p[1] & 0x80))
			starts_new = true;     // first_mb_in_slice == 0: ue "1"
		// the unit's first byte: back over the start code (and a 4th zero)
		usize at = (usize)(nal.p - p);
		usize sc = at >= 3 ? at - 3 : 0;
		if (sc > 0 && p[sc - 1] == 0)
			sc--;
		if (starts_new) {
			out.push_back({begin, sc});
			begin = sc;
			have_slice = false;
		}
		if (is_slice(t))
			have_slice = true;
	}
	if (have_slice || (!nals.empty() && begin < n))
		out.push_back({begin, n});
	return out;
}

namespace {

// Where frame_cropping_flag starts and where its fields end, in RBSP bits
// (crop_sps rewrites what is between).
struct SpsPos {
	usize crop = 0, after_crop = 0;
};

std::string parse_sps_at(const u8 *nal, usize n, Sps &s, SpsPos *at)
{
	s = Sps();
	if (n < 4 || (nal[0] & 0x1f) != kSps)
		return "not an SPS";
	Bits b(nal + 1, n - 1);
	s.profile = (u8)b.u(8);
	s.constraints = (u8)b.u(8);
	s.level = (u8)b.u(8);
	s.id = b.ue();
	switch (s.profile) {
	case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128:
	case 138: case 139: case 134: case 135:
		s.chroma_format = b.ue();
		if (s.chroma_format == 3)
			b.u(1);                         // separate_colour_plane_flag
		b.ue();                             // bit_depth_luma_minus8
		b.ue();                             // bit_depth_chroma_minus8
		b.u(1);                             // qpprime_y_zero_transform_bypass
		if (b.u(1))
			return "SPS with scaling matrices: not supported";
		break;
	default:
		break;
	}
	s.log2_max_frame_num = b.ue() + 4;
	s.poc_type = b.ue();
	if (s.poc_type == 0) {
		s.log2_max_poc_lsb = b.ue() + 4;
	} else if (s.poc_type == 1) {
		s.delta_pic_order_always_zero = b.u(1);
		b.se();
		b.se();
		unsigned cycle = b.ue();
		if (cycle > 255)
			return "SPS: bad POC cycle";
		for (unsigned i = 0; i < cycle; i++)
			b.se();
	} else if (s.poc_type != 2) {
		return "SPS: bad pic_order_cnt_type";
	}
	s.max_ref_frames = b.ue();
	b.u(1);                                 // gaps_in_frame_num_value_allowed
	s.width_mbs = b.ue() + 1;
	unsigned map_units = b.ue() + 1;
	s.frame_mbs_only = b.u(1);
	if (!s.frame_mbs_only)
		b.u(1);                             // mb_adaptive_frame_field
	b.u(1);                                 // direct_8x8_inference
	s.height_mbs = map_units * (s.frame_mbs_only ? 1 : 2);
	s.width = s.width_mbs * 16;
	s.height = s.height_mbs * 16;
	if (at != nullptr)
		at->crop = b.pos();
	if (b.u(1)) {                           // frame_cropping
		unsigned l = b.ue(), r = b.ue(), t = b.ue(), bo = b.ue();
		unsigned cx = s.chroma_format == 0 || s.chroma_format == 3 ? 1 : 2;
		unsigned cy = (s.chroma_format == 1 ? 2 : 1) * (s.frame_mbs_only ? 1 : 2);
		if (cx * (l + r) >= s.width || cy * (t + bo) >= s.height)
			return "SPS: bad cropping";
		s.width -= cx * (l + r);
		s.height -= cy * (t + bo);
	}
	if (at != nullptr)
		at->after_crop = b.pos();
	if (b.fail())
		return "SPS is cut short";
	if (s.log2_max_frame_num > 16 || s.log2_max_poc_lsb > 16)
		return "SPS: bad frame_num / POC sizes";
	return "";
}

// NAL payload -> RBSP: the 03 after two zero bytes is dropped.
std::vector<u8> unescape(const u8 *p, usize n)
{
	std::vector<u8> out;
	unsigned zeros = 0;

	out.reserve(n);
	for (usize i = 0; i < n; i++) {
		if (zeros >= 2 && p[i] == 3) {
			zeros = 0;
			continue;
		}
		out.push_back(p[i]);
		zeros = p[i] == 0 ? zeros + 1 : 0;
	}
	return out;
}

} // namespace

std::string parse_sps(const u8 *nal, usize n, Sps &s)
{
	return parse_sps_at(nal, n, s, nullptr);
}

std::string crop_sps(const u8 *nal, usize n, unsigned width, unsigned height,
                     u8 level, std::vector<u8> &out)
{
	Sps s;
	SpsPos at;
	std::string err = parse_sps_at(nal, n, s, &at);
	if (!err.empty())
		return err;
	if (s.chroma_format != 1 || !s.frame_mbs_only)
		return "crop: 4:2:0 frames only";
	unsigned cw = s.width_mbs * 16, ch = s.height_mbs * 16;
	if (width == 0 || height == 0 || width > cw || height > ch || width % 2 || height % 2)
		return "crop: " + std::to_string(width) + "x" + std::to_string(height) +
		       " does not fit the coded " + std::to_string(cw) + "x" + std::to_string(ch);

	std::vector<u8> rbsp = unescape(nal + 1, n - 1);
	// the last 1 bit is rbsp_stop_one_bit; the fields end before it
	usize end = rbsp.size() * 8;
	while (end > 0 && !(rbsp[(end - 1) / 8] >> (7 - (end - 1) % 8) & 1))
		end--;
	if (end == 0 || end - 1 < at.after_crop)
		return "crop: no rbsp_stop_one_bit after the cropping fields";
	end--;
	if (level != 0)
		rbsp[2] = level;                     // level_idc: byte 2, byte-aligned

	auto bit = [&](usize i) { return (unsigned)(rbsp[i / 8] >> (7 - i % 8) & 1); };
	Writer w;
	for (usize i = 0; i < at.crop; i++)
		w.u(1, bit(i));
	bool crop = width != cw || height != ch;
	w.u(1, crop);
	if (crop) {                              // in 2-pixel units (4:2:0 frames)
		w.ue(0);
		w.ue((cw - width) / 2);
		w.ue(0);
		w.ue((ch - height) / 2);
	}
	for (usize i = at.after_crop; i < end; i++)
		w.u(1, bit(i));
	w.trailing();

	out.clear();
	out.push_back(nal[0]);
	escape(w.out, out);
	return "";
}

std::string crop_sps_annexb(const u8 *p, usize n, unsigned width, unsigned height,
                            u8 level, std::vector<u8> &out)
{
	static const u8 kStart[4] = {0, 0, 0, 1};
	std::vector<u8> sps;

	out.clear();
	for (const Nal &nal : split_annexb(p, n)) {
		out.insert(out.end(), kStart, kStart + 4);
		if (nal.type() != kSps) {
			out.insert(out.end(), nal.p, nal.p + nal.n);
			continue;
		}
		std::string err = crop_sps(nal.p, nal.n, width, height, level, sps);
		if (!err.empty())
			return err;
		out.insert(out.end(), sps.begin(), sps.end());
	}
	return "";
}

std::string parse_pps(const u8 *nal, usize n, Pps &p)
{
	p = Pps();
	if (n < 2 || (nal[0] & 0x1f) != kPps)
		return "not a PPS";
	Bits b(nal + 1, n - 1);
	p.id = b.ue();
	p.sps_id = b.ue();
	p.cabac = b.u(1);
	p.bottom_field_pic_order_present = b.u(1);
	p.slice_groups = b.ue() + 1;
	if (p.slice_groups != 1)
		return "PPS with slice groups: not supported";
	p.num_ref_idx_l0_default = b.ue() + 1;
	b.ue();                                 // num_ref_idx_l1_default_active_minus1
	p.weighted_pred = b.u(1);
	b.u(2);                                 // weighted_bipred_idc
	b.se();                                 // pic_init_qp_minus26
	b.se();                                 // pic_init_qs_minus26
	b.se();                                 // chroma_qp_index_offset
	p.deblocking_control_present = b.u(1);
	b.u(1);                                 // constrained_intra_pred
	p.redundant_pic_cnt_present = b.u(1);
	if (b.fail())
		return "PPS is cut short";
	return "";
}

std::string parse_slice(const u8 *nal, usize n, const Sps &sps, const Pps &pps, Slice &s)
{
	s = Slice();
	if (n < 2 || !is_slice(nal[0] & 0x1f))
		return "not a slice";
	s.nal_type = nal[0] & 0x1f;
	s.ref_idc = (u8)(nal[0] >> 5 & 3);
	Bits b(nal + 1, n - 1);
	s.first_mb = b.ue();
	s.slice_type = b.ue();
	s.pps_id = b.ue();
	if (s.pps_id != pps.id)
		return "slice refers to another PPS";
	s.frame_num = b.u(sps.log2_max_frame_num);
	if (!sps.frame_mbs_only && b.u(1))
		return "field pictures: not supported";
	if (s.idr())
		s.idr_pic_id = b.ue();
	if (sps.poc_type == 0)
		s.poc_lsb = b.u(sps.log2_max_poc_lsb);
	if (b.fail())
		return "slice header is cut short";
	return "";
}

std::string skip_supported(const Sps &sps, const Pps &pps)
{
	if (pps.cabac)
		return "CABAC (not Baseline)";
	if (pps.weighted_pred)
		return "weighted prediction";
	if (!sps.frame_mbs_only)
		return "field coding";
	if (sps.chroma_format == 3)
		return "4:4:4";
	if (pps.sps_id != sps.id)
		return "PPS refers to another SPS";
	return "";
}

void escape(const std::vector<u8> &rbsp, std::vector<u8> &out)
{
	unsigned zeros = 0;

	for (u8 b : rbsp) {
		if (zeros >= 2 && b <= 3) {
			out.push_back(3);
			zeros = 0;
		}
		out.push_back(b);
		zeros = b == 0 ? zeros + 1 : 0;
	}
}

std::vector<u8> skip_slice(const Sps &sps, const Pps &pps, unsigned frame_num,
                                unsigned poc_lsb)
{
	Writer w;

	w.ue(0);                                // first_mb_in_slice
	w.ue(5);                                // slice_type: P, every slice of the picture P
	w.ue(pps.id);
	w.u(sps.log2_max_frame_num, frame_num & ((1u << sps.log2_max_frame_num) - 1));
	if (sps.poc_type == 0) {
		w.u(sps.log2_max_poc_lsb, poc_lsb & ((1u << sps.log2_max_poc_lsb) - 1));
		if (pps.bottom_field_pic_order_present)
			w.se(0);                        // delta_pic_order_cnt_bottom
	} else if (sps.poc_type == 1 && !sps.delta_pic_order_always_zero) {
		w.se(0);                            // delta_pic_order_cnt[0]
		if (pps.bottom_field_pic_order_present)
			w.se(0);
	}
	if (pps.redundant_pic_cnt_present)
		w.ue(0);
	w.u(1, 0);                              // num_ref_idx_active_override_flag
	w.u(1, 0);                              // ref_pic_list_modification_flag_l0
	// nal_ref_idc 0: no dec_ref_pic_marking
	w.se(0);                                // slice_qp_delta
	if (pps.deblocking_control_present)
		w.ue(1);                            // disable_deblocking_filter_idc: off
	w.ue(sps.mbs());                        // mb_skip_run: the whole picture
	w.trailing();

	std::vector<u8> nal;
	nal.push_back(kSlice);                  // forbidden 0, nal_ref_idc 0, type 1
	escape(w.out, nal);
	return nal;
}

} // namespace h264
