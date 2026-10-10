/*
 * gf/gf.h - QNX Graphics Framework (GF), QNX 6.3.2 - minimal reconstruction.
 *
 * Covers only what our apps call. Sources:
 *  - function signatures: QNX 6.4.1 GF developer guide (API reference)
 *  - struct layouts + gf_format_t values: STABS debug info of qnx-audi's
 *    dbg-shle.elf, which was compiled against the real QNX 6.3.2 headers.
 *    The 6.4 docs differ here (6.3.2 has reserved[] instead of
 *    vendor_id/device_id, and vaddr precedes paddr in gf_surface_info_t).
 *  - GF_ERR_*, GF_DEVICE_INDEX(0), GF_LAYER_ATTACH_PASSIVE: disassembly of
 *    qnx-audi's Screen::attach* switch tables / call sites.
 * The _Static_asserts at the bottom pin every layout to the STABS data.
 * uint64_t members rely on the sysroot's 6.3.2-style 4-byte alignment of
 * 64-bit types (see build-sysroot.sh); with 6.4 headers they would not fit.
 */
#ifndef _GF_GF_H_INCLUDED
#define _GF_GF_H_INCLUDED

#include <stdint.h>

typedef struct _gf_dev     *gf_dev_t;
typedef struct _gf_display *gf_display_t;
typedef struct _gf_layer   *gf_layer_t;
typedef struct _gf_surface *gf_surface_t;
typedef unsigned            gf_sid_t;

/* Return codes (GF_ERR_OK == 0; numbering decoded from qnx-audi). */
#define GF_ERR_OK         0
#define GF_ERR_MEM        1
#define GF_ERR_IODISPLAY  2
#define GF_ERR_DEVICE     3
#define GF_ERR_SHMEM      4
#define GF_ERR_DLL        5
#define GF_ERR_THREAD     6
#define GF_ERR_PARM       7
#define GF_ERR_INUSE      8
#define GF_ERR_NOSUPPORT  9
#define GF_ERR_CFG        10

/* Select a device by index in /dev/io-display. Only index 0 is verified
 * (qnx-audi passes a NULL name for GF_DEVICE_INDEX(0)). */
#define GF_DEVICE_INDEX(n)        ((const char *)(uintptr_t)(n))

#define GF_LAYER_ATTACH_PASSIVE   0x00000001u

typedef enum {
	GF_FORMAT_INVALID         = 0,
	GF_FORMAT_BYTE            = 8,
	GF_FORMAT_PAL8            = 2056,
	GF_FORMAT_PACK_ARGB1555   = 5904,
	GF_FORMAT_PKLE_ARGB1555   = 5392,
	GF_FORMAT_PKBE_ARGB1555   = 5648,
	GF_FORMAT_PACK_RGB565     = 4880,
	GF_FORMAT_PKLE_RGB565     = 4368,
	GF_FORMAT_PKBE_RGB565     = 4624,
	GF_FORMAT_BGR888          = 4120,
	GF_FORMAT_BGRA8888        = 5152,
	GF_FORMAT_ARGB8888        = 5664,
	GF_FORMAT_PACK_YUV_UYVY   = 16786192,
	GF_FORMAT_PKLE_YUV_UYVY   = 16785680,
	GF_FORMAT_PKBE_YUV_UYVY   = 16785936,
	GF_FORMAT_PACK_YUV_YUY2   = 33563408,
	GF_FORMAT_PKLE_YUV_YUY2   = 33562896,
	GF_FORMAT_PKBE_YUV_YUY2   = 33563152,
	GF_FORMAT_PACK_YUV_YVYU   = 50340624,
	GF_FORMAT_PKLE_YUV_YVYU   = 50340112,
	GF_FORMAT_PKBE_YUV_YVYU   = 50340368,
	GF_FORMAT_PACK_YUV_V422   = 67117840,
	GF_FORMAT_PKLE_YUV_V422   = 67117328,
	GF_FORMAT_PKBE_YUV_V422   = 67117584,
	GF_FORMAT_PACK_YUV_NV12   = 83894284,
	GF_FORMAT_PACK_YUV_AYUV   = 100671520,
	GF_FORMAT_PLANAR_YUV_YVU9 = 16785544,
	GF_FORMAT_PLANAR_YUV_YV12 = 33562760,
	GF_FORMAT_PLANAR_YUV_420  = 50339976
} gf_format_t;

typedef struct {
	int       ncolors;
	uint32_t *colors;
} gf_palette_t;

typedef struct {
	unsigned    ndisplays;
	unsigned    flags;
	const char *description;
	int         reserved[6];
} gf_dev_info_t;

typedef struct {
	unsigned    nlayers;
	unsigned    main_layer_index;
	uint16_t    xres;
	uint16_t    yres;
	gf_format_t format;
	int         refresh;
	int         reserved[6];
} gf_display_info_t;

typedef struct {
	gf_format_t format;
	unsigned    caps;
	unsigned    alpha_valid_flags;
	unsigned    alpha_combinations;
	uint64_t    order_caps;
	unsigned    chromakey_caps;
	int         src_max_height;
	int         src_max_width;
	int         src_max_viewport_height;
	int         src_max_viewport_width;
	int         dst_max_height;
	int         dst_max_width;
	int         dst_min_height;
	int         dst_min_width;
	int         max_scaleup_x;
	int         max_scaleup_y;
	int         max_scaledown_x;
	int         max_scaledown_y;
	unsigned    output_mask;
	unsigned    vcap_mask;
	int         reserved[6];
} gf_layer_info_t;

typedef struct {
	gf_sid_t     sid;
	int          w;
	int          h;
	gf_format_t  format;
	unsigned     stride;
	uint8_t     *vaddr;
	uint64_t     paddr;
	gf_palette_t palette;
	unsigned     flags;
	int          reserved[6];
} gf_surface_info_t;

int  gf_dev_attach(gf_dev_t *pdev, const char *name, gf_dev_info_t *info);
void gf_dev_detach(gf_dev_t gdev);

int  gf_display_attach(gf_display_t *pdisplay, gf_dev_t gdev,
                       unsigned display_index, gf_display_info_t *info);
void gf_display_detach(gf_display_t display);

int  gf_layer_attach(gf_layer_t *player, gf_display_t display,
                     unsigned layer_index, unsigned flags);
void gf_layer_detach(gf_layer_t layer);
int  gf_layer_query(gf_layer_t layer, int format_index, gf_layer_info_t *info);
void gf_layer_set_src_viewport(gf_layer_t layer, int x1, int y1, int x2, int y2);
void gf_layer_set_dst_viewport(gf_layer_t layer, int x1, int y1, int x2, int y2);
void gf_layer_enable(gf_layer_t layer);
void gf_layer_disable(gf_layer_t layer);

int  gf_surface_attach_by_sid(gf_surface_t *psurface, gf_dev_t gdev, gf_sid_t sid);
void gf_surface_free(gf_surface_t surface);
gf_surface_info_t *gf_surface_get_info(gf_surface_t surface, gf_surface_info_t *info);

/* Layout checks against QNX 6.3.2 (sizes/offsets from STABS, in bytes). */
_Static_assert(sizeof(gf_dev_info_t) == 36, "gf_dev_info_t");
_Static_assert(sizeof(gf_display_info_t) == 44, "gf_display_info_t");
_Static_assert(__builtin_offsetof(gf_display_info_t, xres) == 8, "xres");
_Static_assert(__builtin_offsetof(gf_display_info_t, format) == 12, "format");
_Static_assert(sizeof(gf_layer_info_t) == 108, "gf_layer_info_t");
_Static_assert(__builtin_offsetof(gf_layer_info_t, chromakey_caps) == 24, "chromakey_caps");
_Static_assert(__builtin_offsetof(gf_layer_info_t, vcap_mask) == 80, "vcap_mask");
_Static_assert(sizeof(gf_surface_info_t) == 68, "gf_surface_info_t");
_Static_assert(__builtin_offsetof(gf_surface_info_t, paddr) == 24, "paddr");
_Static_assert(__builtin_offsetof(gf_surface_info_t, flags) == 40, "flags");

#endif
