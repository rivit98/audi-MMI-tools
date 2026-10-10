#ifndef RIVIT_BMP_H
#define RIVIT_BMP_H

/* Write w x h RGBA8888 pixels as a 24-bit BMP. Rows are bottom-up, which is
 * what both glReadPixels and BMP use. Returns 0 or -1 (errno set). */
int bmp_write_rgba(const char *path, const unsigned char *rgba, int w, int h);

#endif
