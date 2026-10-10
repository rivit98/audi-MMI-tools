/*
 * gf/gf3d.h - QNX GF 3D render targets (QNX 6.3.2) - minimal reconstruction.
 * A gf_3d_target_t is what EGL takes as EGLNativeWindowType on QNX GF.
 */
#ifndef _GF_GF3D_H_INCLUDED
#define _GF_GF3D_H_INCLUDED

#include <gf/gf.h>

typedef struct _gf_3d_target *gf_3d_target_t;

/* surfaces == NULL: GF allocates its own buffers; otherwise render into the
 * nsurfaces given (the MMI apps pass the layer manager's VFB surface). */
int  gf_3d_target_create(gf_3d_target_t *ptarget, gf_layer_t layer,
                         gf_surface_t *surfaces, int nsurfaces,
                         int width, int height, gf_format_t format);
void gf_3d_target_free(gf_3d_target_t target);

#endif
