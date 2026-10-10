/*
 * crtarray.c - init/fini array runners expected by QNX's SH crt1.o.
 *
 * openqnx's lib/c/startup/sh/crt1.S calls _preinit_array(), _init_array()
 * and _fini_array() with linker-provided [start, end) bounds. On a real QNX
 * SDK these come from the static part of libc (libc.a / crt objects), which
 * is not in openqnx and not exported by the device's libc.so.2, so we
 * provide them here. GCC >= 4.7 emits constructors into .init_array, so
 * __attribute__((constructor)) works only because of these.
 */
#include <stdlib.h>

typedef void (*crt_fn)(void);

static crt_fn *fini_start, *fini_end;

void _preinit_array(crt_fn *start, crt_fn *end)
{
	while (start < end)
		(*start++)();
}

void _init_array(crt_fn *start, crt_fn *end)
{
	while (start < end)
		(*start++)();
}

static void run_fini_array(void)
{
	/* destructors run in reverse order of construction */
	while (fini_end > fini_start)
		(*--fini_end)();
}

void _fini_array(crt_fn *start, crt_fn *end)
{
	fini_start = start;
	fini_end = end;
	if (start < end)
		atexit(run_fini_array);
}
