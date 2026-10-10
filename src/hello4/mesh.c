/*
 * mesh.c - cube, torus and ground grid (see mesh.h).
 */
#include "mesh.h"

#include <stdlib.h>
#include <string.h>

#define TWO_PI 6.2831853f

void mesh_free(struct mesh *m)
{
	free(m->pos);
	free(m->nrm);
	free(m->uv);
	free(m->col);
	free(m->idx);
	memset(m, 0, sizeof(*m));
}

static int mesh_alloc(struct mesh *m, int nverts, int nidx, int with_col)
{
	memset(m, 0, sizeof(*m));
	if (nverts > 65536)
		return -1;           /* GL_UNSIGNED_SHORT indices */
	m->nverts = nverts;
	m->nidx = nidx;
	m->pos = malloc(nverts * 3 * sizeof(GLfloat));
	m->nrm = malloc(nverts * 3 * sizeof(GLfloat));
	m->uv = malloc(nverts * 2 * sizeof(GLfloat));
	m->col = with_col ? malloc(nverts * 4) : NULL;
	m->idx = malloc(nidx * sizeof(GLushort));
	if (!m->pos || !m->nrm || !m->uv || (with_col && !m->col) || !m->idx) {
		mesh_free(m);
		return -1;
	}
	return 0;
}

/* Quad a-b-c-d (counter-clockwise) as two triangles. */
static GLushort *quad_idx(GLushort *idx, int a, int b, int c, int d)
{
	idx[0] = a; idx[1] = b; idx[2] = c;
	idx[3] = a; idx[4] = c; idx[5] = d;
	return idx + 6;
}

int mesh_cube(struct mesh *m)
{
	/* Per face: normal n, then u and v with u x v = n, so the corners
	 * n + s*u + t*v in the order (-,-) (+,-) (+,+) (-,+) are
	 * counter-clockwise seen from outside. */
	static const signed char face[6][9] = {
		{  1, 0, 0,   0, 0,-1,   0, 1, 0 },
		{ -1, 0, 0,   0, 0, 1,   0, 1, 0 },
		{  0, 1, 0,   1, 0, 0,   0, 0,-1 },
		{  0,-1, 0,   1, 0, 0,   0, 0, 1 },
		{  0, 0, 1,   1, 0, 0,   0, 1, 0 },
		{  0, 0,-1,  -1, 0, 0,   0, 1, 0 },
	};
	static const GLubyte colour[6][3] = {
		{ 230, 40, 40 }, { 40, 210, 220 }, { 50, 210, 60 },
		{ 220, 50, 210 }, { 50, 80, 235 }, { 240, 210, 40 },
	};
	static const signed char st[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
	GLushort *idx;
	int f, k, i;

	if (mesh_alloc(m, 24, 36, 1))
		return -1;
	idx = m->idx;
	for (f = 0; f < 6; f++) {
		const signed char *n = face[f], *u = face[f] + 3, *v = face[f] + 6;

		for (k = 0; k < 4; k++) {
			int vi = f * 4 + k;

			for (i = 0; i < 3; i++) {
				m->pos[vi * 3 + i] = n[i] + st[k][0] * u[i] + st[k][1] * v[i];
				m->nrm[vi * 3 + i] = n[i];
				/* the (+,+) corner is halfway to white */
				m->col[vi * 4 + i] = k == 2 ? (colour[f][i] + 255) / 2 : colour[f][i];
			}
			m->col[vi * 4 + 3] = 255;
			m->uv[vi * 2] = 0.5f * (st[k][0] + 1);
			m->uv[vi * 2 + 1] = 0.5f * (st[k][1] + 1);
		}
		idx = quad_idx(idx, f * 4, f * 4 + 1, f * 4 + 2, f * 4 + 3);
	}
	return 0;
}

int mesh_torus(struct mesh *m, float R, float r, int nu, int nv)
{
	GLushort *idx;
	int i, j, k = 0;

	/* the seam vertices are duplicated so the texture wraps cleanly */
	if (mesh_alloc(m, (nu + 1) * (nv + 1), nu * nv * 6, 0))
		return -1;
	for (i = 0; i <= nu; i++) {
		float th = TWO_PI * i / nu, ct = cosf(th), st = sinf(th);

		for (j = 0; j <= nv; j++, k++) {
			float ph = TWO_PI * j / nv, cp = cosf(ph), sp = sinf(ph);

			m->pos[k * 3]     = (R + r * cp) * ct;
			m->pos[k * 3 + 1] = (R + r * cp) * st;
			m->pos[k * 3 + 2] = r * sp;
			m->nrm[k * 3]     = cp * ct;
			m->nrm[k * 3 + 1] = cp * st;
			m->nrm[k * 3 + 2] = sp;
			m->uv[k * 2]      = 4.0f * i / nu;
			m->uv[k * 2 + 1]  = (float)j / nv;
		}
	}
	/* +i then +j turns counter-clockwise around the outward normal */
	idx = m->idx;
	for (i = 0; i < nu; i++)
		for (j = 0; j < nv; j++)
			idx = quad_idx(idx, i * (nv + 1) + j, (i + 1) * (nv + 1) + j,
			               (i + 1) * (nv + 1) + j + 1, i * (nv + 1) + j + 1);
	return 0;
}

int mesh_grid(struct mesh *m, float size, int n, float uv_repeat)
{
	GLushort *idx;
	int i, j, k = 0;

	if (mesh_alloc(m, (n + 1) * (n + 1), n * n * 6, 0))
		return -1;
	for (i = 0; i <= n; i++) {           /* i along x */
		for (j = 0; j <= n; j++, k++) {  /* j along z */
			m->pos[k * 3]     = size * ((float)i / n - 0.5f);
			m->pos[k * 3 + 1] = 0.0f;
			m->pos[k * 3 + 2] = size * ((float)j / n - 0.5f);
			m->nrm[k * 3]     = 0.0f;
			m->nrm[k * 3 + 1] = 1.0f;
			m->nrm[k * 3 + 2] = 0.0f;
			m->uv[k * 2]      = uv_repeat * i / n;
			m->uv[k * 2 + 1]  = uv_repeat * j / n;
		}
	}
	/* +z then +x is counter-clockwise seen from above */
	idx = m->idx;
	for (i = 0; i < n; i++)
		for (j = 0; j < n; j++)
			idx = quad_idx(idx, i * (n + 1) + j, i * (n + 1) + j + 1,
			               (i + 1) * (n + 1) + j + 1, (i + 1) * (n + 1) + j);
	return 0;
}

static void client_array(GLenum array, int on)
{
	if (on)
		glEnableClientState(array);
	else
		glDisableClientState(array);
}

void mesh_draw(const struct mesh *m, unsigned arrays)
{
	int col = (arrays & MESH_COLOR) && m->col;

	glEnableClientState(GL_VERTEX_ARRAY);
	glVertexPointer(3, GL_FLOAT, 0, m->pos);
	client_array(GL_NORMAL_ARRAY, arrays & MESH_NORMAL);
	client_array(GL_TEXTURE_COORD_ARRAY, arrays & MESH_UV);
	client_array(GL_COLOR_ARRAY, col);
	if (arrays & MESH_NORMAL)
		glNormalPointer(GL_FLOAT, 0, m->nrm);
	if (arrays & MESH_UV)
		glTexCoordPointer(2, GL_FLOAT, 0, m->uv);
	if (col)
		glColorPointer(4, GL_UNSIGNED_BYTE, 0, m->col);
	glDrawElements(GL_TRIANGLES, m->nidx, GL_UNSIGNED_SHORT, m->idx);
}
