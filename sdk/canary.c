/*
 * canary.c - build-time check for the sh4 GCC miscompile described in
 * docs/gcc-sh4-treg-bug.md. Debian's sh4-linux-gnu-gcc 12.2.0 at -O1 and
 * above drops the second test here (*p == 0), so the function returns 22
 * whenever a == 0; GCC 16.2 compiles it correctly. The Makefile compiles
 * this with the apps' CFLAGS and refuses to build if the assembly has fewer
 * than two compares.
 */
int canary_and(int a, const int *p)
{
	if (a == 0 && *p == 0)
		return 22;
	return a;
}
