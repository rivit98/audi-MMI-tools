/*
 * test_layout.c - pngondemand's layout language (make test).
 */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "font_big.h"
#include "font_small.h"
#include "layout.h"

static struct layout_result r;

static int render(const char *src)
{
	free(r.canvas.px);
	return layout_render(src, &r);
}

static uint32_t px(int x, int y)
{
	return r.canvas.px[y * r.canvas.stride + x];
}

/* Foreground pixels of colour rgb in the rectangle. */
static int count(int x0, int y0, int w, int h, uint32_t rgb)
{
	int x, y, n = 0;

	for (y = y0; y < y0 + h; y++)
		for (x = x0; x < x0 + w; x++)
			n += px(x, y) == (0xFF000000u | rgb);
	return n;
}

static void test_canvas(void)
{
	CHECK_EQ(render(""), 0);
	CHECK_EQ(r.canvas.w, 800);
	CHECK_EQ(r.canvas.h, 480);
	CHECK_EQ(px(0, 0), 0xFF000000u);
	CHECK_EQ(px(799, 479), 0xFF000000u);

	CHECK_EQ(render("# comment\n\n  size 40 30\r\nbg #102030\n"), 0);
	CHECK_EQ(r.canvas.w, 40);
	CHECK_EQ(r.canvas.h, 30);
	CHECK_EQ(px(39, 29), 0xFF102030u);

	CHECK_EQ(render("size 20 10\ncolour accent 5ac8fa\nrect 5 2 3 4 accent\n"
	                "colour accent 010203\nrect -5 -5 6 6 accent"), 0);
	CHECK_EQ(count(0, 0, 20, 10, 0x5AC8FA), 12);
	CHECK_EQ(px(5, 2), 0xFF5AC8FAu);
	CHECK_EQ(px(7, 5), 0xFF5AC8FAu);
	CHECK_EQ(px(8, 5), 0xFF000000u);
	CHECK_EQ(count(0, 0, 20, 10, 0x010203), 1);   /* clipped to 0,0 */
	CHECK_EQ(r.dropped, 0);
}

static void test_errors(void)
{
	CHECK_EQ(render("size 10 10\nbg ffffff\nsize 20 20\n"), -1);
	CHECK_EQ(r.err_line, 3);
	CHECK(strstr(r.err, "before") != NULL);
	CHECK(r.canvas.px == NULL);

	CHECK_EQ(render("bogus 1 2"), -1);
	CHECK_EQ(r.err_line, 1);
	CHECK_STR(r.err, "unknown command \"bogus\"");

	CHECK_EQ(render("\n\nrect 1 2 3 nope red"), -1);
	CHECK_EQ(r.err_line, 3);
	CHECK_STR(r.err, "bad height \"nope\"");
	CHECK_EQ(render("rect 1 2 3 4"), -1);
	CHECK_STR(r.err, "missing colour");
	CHECK_EQ(render("rect 1 2 3 4 red"), -1);
	CHECK_STR(r.err, "unknown colour \"red\"");
	CHECK_EQ(render("bg 12345"), -1);
	CHECK_EQ(render("bg 1234567"), -1);
	CHECK_EQ(render("bg ffffff extra"), -1);
	CHECK_STR(r.err, "unexpected \"extra\"");
	CHECK_EQ(render("line huge ffffff hi"), -1);
	CHECK_STR(r.err, "unknown font \"huge\" (small or big)");
	CHECK_EQ(render("size 0 10"), -1);
	CHECK_EQ(render("size 4000 10"), -1);
	CHECK_EQ(render("colour abcdef 123456"), -1);
	CHECK_EQ(render("colour averyverylongname 123456"), -1);
}

static void test_text(void)
{
	int lit;

	/* text: glyphs at x,y in the colour, nothing else touched */
	CHECK_EQ(render("size 100 40\ntext 10 4 small ffffff H"), 0);
	lit = count(10, 4, 8, 16, 0xFFFFFF);
	CHECK(lit > 10);
	CHECK_EQ(count(0, 0, 100, 40, 0xFFFFFF), lit);

	/* the text is the rest of the line: leading spaces indent it */
	CHECK_EQ(render("size 100 40\nmargin 0\nat 0\nline small ffffff   H"), 0);
	CHECK_EQ(count(16, 0, 8, 16, 0xFFFFFF), lit);   /* 2 spaces */
	CHECK_EQ(count(0, 0, 16, 16, 0xFFFFFF), 0);

	/* flow: line advances by the font height, gap by N */
	CHECK_EQ(render("size 100 100\nmargin 0\nat 0\nline small ffffff H\ngap 4\n"
	                "line big ffffff H\nline small ffffff H"), 0);
	CHECK_EQ(count(0, 0, 8, 16, 0xFFFFFF), lit);
	CHECK(count(0, 20, 16, 32, 0xFFFFFF) > lit);
	CHECK_EQ(count(0, 52, 8, 16, 0xFFFFFF), lit);
	CHECK_EQ(r.dropped, 0);

	/* an empty line still advances */
	CHECK_EQ(render("size 100 100\nmargin 0\nat 0\nline small ffffff\n"
	                "line small ffffff H"), 0);
	CHECK_EQ(count(0, 16, 8, 16, 0xFFFFFF), lit);

	/* cut at the right margin: 100 - 2*10 = 80 px = 10 small chars */
	CHECK_EQ(render("size 100 40\nmargin 10\nline small ffffff HHHHHHHHHHHH"), 0);
	CHECK_EQ(r.cut, 1);
	CHECK_EQ(count(0, 0, 100, 40, 0xFFFFFF), 10 * lit);
	CHECK_EQ(render("size 100 40\nmargin 10\nline small ffffff HHHHHHHHHH"), 0);
	CHECK_EQ(r.cut, 0);
}

static void test_flow_limits(void)
{
	/* 50 px canvas, 16 px lines from 0: three fit, two dropped */
	CHECK_EQ(render("size 50 50\nat 0\nline small ffffff a\nline small ffffff a\n"
	                "line small ffffff a\nline small ffffff a\nline small ffffff a"), 0);
	CHECK_EQ(r.dropped, 2);

	/* the footer moves the limit up: 50 - 20 = 30 -> one line fits */
	CHECK_EQ(render("size 50 50\nat 0\nfooter 20 small 112233 ffffff\n"
	                "line small ffffff a\nline small ffffff a"), 0);
	CHECK_EQ(r.dropped, 1);
	CHECK_EQ(px(0, 30), 0xFF112233u);
	CHECK_EQ(px(49, 49), 0xFF112233u);
	CHECK_EQ(px(0, 29), 0xFF000000u);
}

static int lines_with(uint32_t rgb, int x0, int w, int h)
{
	int y, n = 0;

	for (y = 0; y + 16 <= h; y += 16)
		n += count(x0, y, w, 16, rgb) > 0;
	return n;
}

static void test_para(void)
{
	/* 100 - 2*2 = 96 px = 12 cols */
	CHECK_EQ(render("size 100 200\nmargin 2\nat 0\n"
	                "para small ffffff aaaa bbbb cccc dddd eeee"), 0);
	CHECK_EQ(lines_with(0xFFFFFF, 0, 100, 200), 3);   /* "aaaa bbbb" x2, "eeee" */

	/* "words 30 s" would break as "words 30" / "s"; it is kept together */
	CHECK_EQ(render("size 100 200\nmargin 2\nat 0\npara small ffffff abcdefgh 30 s"), 0);
	CHECK_EQ(lines_with(0xFFFFFF, 0, 100, 200), 2);
	CHECK_EQ(count(2, 0, 8 * 9, 16, 0xFFFFFF), count(2, 0, 96, 16, 0xFFFFFF));
	CHECK(count(2, 16, 8 * 4, 16, 0xFFFFFF) > 0);     /* "30 s" on line 2 */

	CHECK_EQ(render("size 100 40\nat 0\npara small ffffff a b c d e f g h i j k l m n"
	                " o p q r s t u v w x y z"), 0);
	CHECK(r.dropped > 0);
}

int main(void)
{
	test_canvas();
	test_errors();
	test_text();
	test_flow_limits();
	test_para();
	free(r.canvas.px);
	CHECK_DONE();
}
