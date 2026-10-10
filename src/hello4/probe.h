/*
 * probe.h - automatic checks of the fixed-function 3D pipeline.
 *
 * Each probe draws a few quads into the back buffer (never swapped, so
 * nothing shows on screen), reads chosen pixels back with glReadPixels
 * and compares them with what the GL spec says they must be. Every read
 * and the verdict go to the log:
 *
 *   clear    glClear colour readback, corners included (readback works)
 *   depth    GL_LESS rejects a farther quad drawn later; glDepthMask(FALSE)
 *   cull     back faces culled; glFrontFace(GL_CW) flips it
 *   persp    glFrustumf: quads at z=-2/-4 land where the projection says
 *   light    directional diffuse: N.L = 1, 0.707 and 0 (back-facing)
 *   texture  2x2 GL_NEAREST texture, GL_REPLACE: all four texels
 *   fog      linear fog: full, half and none
 *   blend    GL_SRC_ALPHA / GL_ONE_MINUS_SRC_ALPHA at alpha 0.5
 *   stencil  draw only where a stencil mask was written (SKIP if the EGL
 *            config has no stencil bits)
 *
 * Colours are compared with a tolerance for a 16-bit (RGB565) surface.
 */
#ifndef HELLO4_PROBE_H
#define HELLO4_PROBE_H

enum { PROBE_SKIP, PROBE_PASS, PROBE_FAIL };

#define PROBE_MAX 16

/* Runs every probe on a w x h surface; state[i] gets PROBE_*. Leaves the
 * GL state reset (no tests enabled, identity matrices). Returns the
 * number of probes. */
int probes_run(int w, int h, unsigned char state[PROBE_MAX]);

#endif
