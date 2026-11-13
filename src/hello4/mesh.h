/*
 * mesh.h - indexed triangle meshes, built once by the CPU and drawn from
 * client-side vertex arrays (the unit's ES-CM 1.0 has no buffer objects:
 * libGLES_CM.so.6 exports no glBindBuffer).
 *
 * Every mesh has positions, normals and texcoords; the cube also has
 * per-vertex colours. Triangles are counter-clockwise seen from outside,
 * so they work with the default glFrontFace(GL_CCW) + glCullFace(GL_BACK).
 */
#ifndef HELLO4_MESH_H
#define HELLO4_MESH_H

#include "types.h"
#include <GLES/gl.h>

/* openqnx ships no <math.h> (libm is not in the source leak); the device's
 * libm.so.2 exports these, and the sysroot has stubs for them. */
f32 sinf(f32 x);
f32 cosf(f32 x);

/* Vertex arrays mesh_draw() enables besides positions. */
enum { MESH_NORMAL = 1, MESH_UV = 2, MESH_COLOR = 4 };

struct mesh {
	GLfloat *pos, *nrm, *uv;   /* 3, 3, 2 floats per vertex */
	GLubyte *col;              /* RGBA per vertex, or NULL */
	GLushort *idx;             /* GL_TRIANGLES */
	int nverts, nidx;
};

/* 2x2x2 cube at the origin; one colour per face, lighter at one corner so
 * Gouraud interpolation is visible. */
int  mesh_cube(struct mesh *m);

/* Torus around the z axis: major radius R, tube radius r, nu x nv quads. */
int  mesh_torus(struct mesh *m, f32 R, f32 r, int nu, int nv);

/* size x size square in the y=0 plane, facing +y, split into n x n quads;
 * the texture repeats uv_repeat times across it. */
int  mesh_grid(struct mesh *m, f32 size, int n, f32 uv_repeat);

void mesh_free(struct mesh *m);

/* glDrawElements with the given MESH_* arrays enabled, the others off.
 * Without MESH_COLOR the current glColor4f applies (set it first: it is
 * undefined after a draw with the colour array). */
void mesh_draw(const struct mesh *m, unsigned arrays);

#endif
