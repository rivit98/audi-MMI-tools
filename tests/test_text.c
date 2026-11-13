/* test_text.c - UTF-8, glyph lookup, wrapping, drawing (src/common/text.c). */
#include "types.h"
#include <stdlib.h>

#include "check.h"
#include "text.h"

/* 4x2 test font: glyph 0 = '?' (all set), 1 = 'A' (left column), 2 = 'ż'
 * (top row). */
static const u8 tbits[] = {
	0xF0, 0xF0,
	0x80, 0x80,
	0xF0, 0x00,
};
static const u16 tcp[] = { '?', 'A', 0x017C };   /* sorted */
static const u16 tgl[] = { 0, 1, 2 };
static const struct font tfont = { 4, 2, 1, tbits, tcp, tgl, 3, 0 };

static void test_utf8(void)
{
	const char *s = "Aż€\xF0\x9F\x98\x80";
	const char *p = s;

	CHECK_EQ(utf8_next(&p, NULL), 'A');
	CHECK_EQ(utf8_next(&p, NULL), 0x017C);
	CHECK_EQ(utf8_next(&p, NULL), 0x20AC);
	CHECK_EQ(utf8_next(&p, NULL), 0x1F600);
	CHECK_EQ(utf8_next(&p, NULL), 0);
	CHECK(p == s + strlen(s));

	/* malformed: lone continuation, truncated 2-byte, overlong */
	p = "\x80x";
	CHECK_EQ(utf8_next(&p, NULL), 0xFFFD);
	CHECK_EQ(utf8_next(&p, NULL), 'x');
	p = "\xC5";
	CHECK_EQ(utf8_next(&p, NULL), 0xFFFD);
	CHECK_EQ(utf8_next(&p, NULL), 0);
	p = "\xC0\xAF";
	CHECK_EQ(utf8_next(&p, NULL), 0xFFFD);

	/* explicit end cuts a sequence */
	s = "\xC5\xBC";
	p = s;
	CHECK_EQ(utf8_next(&p, s + 1), 0xFFFD);
	CHECK(p == s + 1);
	CHECK_EQ(utf8_next(&p, s + 1), 0);

	CHECK_EQ(utf8_count("zażółć", -1), 6);
	CHECK_EQ(utf8_count("zażółć", 4), 3);
	CHECK_EQ(utf8_count("zażółć", 3), 3);   /* z, a, cut ż = U+FFFD */
}

static void test_glyph(void)
{
	CHECK_EQ(font_glyph(&tfont, 'A'), 1);
	CHECK_EQ(font_glyph(&tfont, 0x017C), 2);
	CHECK_EQ(font_glyph(&tfont, 'B'), 0);
	CHECK_EQ(font_glyph(&tfont, 0x1F600), 0);
}

struct lines {
	int n;
	char l[16][64];
};

static void add_line(const char *line, int nbytes, void *arg)
{
	struct lines *ls = arg;

	if (ls->n < 16) {
		memcpy(ls->l[ls->n], line, (usize)nbytes);
		ls->l[ls->n][nbytes] = 0;
	}
	ls->n++;
}

static struct lines wrap(const char *s, int cols)
{
	struct lines ls;
	int n;

	memset(&ls, 0, sizeof(ls));
	n = text_wrap(s, cols, add_line, &ls);
	CHECK_EQ(n, ls.n);
	return ls;
}

static void test_wrap(void)
{
	struct lines ls;

	ls = wrap("hello world", 20);
	CHECK_EQ(ls.n, 1);
	CHECK_STR(ls.l[0], "hello world");

	ls = wrap("hello world foo", 11);
	CHECK_EQ(ls.n, 2);
	CHECK_STR(ls.l[0], "hello world");
	CHECK_STR(ls.l[1], "foo");

	ls = wrap("hello world", 7);
	CHECK_EQ(ls.n, 2);
	CHECK_STR(ls.l[0], "hello");
	CHECK_STR(ls.l[1], "world");

	ls = wrap("abcdefghij", 4);              /* hard split */
	CHECK_EQ(ls.n, 3);
	CHECK_STR(ls.l[0], "abcd");
	CHECK_STR(ls.l[1], "efgh");
	CHECK_STR(ls.l[2], "ij");

	ls = wrap("a\nb\r\n\nc", 10);             /* newlines, CR dropped */
	CHECK_EQ(ls.n, 4);
	CHECK_STR(ls.l[0], "a");
	CHECK_STR(ls.l[1], "b");
	CHECK_STR(ls.l[2], "");
	CHECK_STR(ls.l[3], "c");

	ls = wrap("zażółć gęślą", 6);            /* counts codepoints, not bytes */
	CHECK_EQ(ls.n, 2);
	CHECK_STR(ls.l[0], "zażółć");
	CHECK_STR(ls.l[1], "gęślą");

	ls = wrap("", 5);
	CHECK_EQ(ls.n, 1);
	CHECK_STR(ls.l[0], "");

	ls = wrap("a\tb", 5);                     /* text_draw shows '\t' as ' ' */
	CHECK_EQ(ls.n, 1);
	CHECK_STR(ls.l[0], "a\tb");
	ls = wrap("ab\tcd", 3);                   /* ...and wrap breaks at it */
	CHECK_EQ(ls.n, 2);
	CHECK_STR(ls.l[0], "ab");
	CHECK_STR(ls.l[1], "cd");
}

static void test_draw(void)
{
	u32 px[6 * 3];
	struct canvas c = { px, 6, 3, 6 };
	int x;

	memset(px, 0, sizeof(px));
	x = text_draw(&c, &tfont, 0, 0, "Aż", -1, 0xFFFFFFFF);
	CHECK_EQ(x, 8);
	/* 'A': left column of rows 0 and 1 */
	CHECK_EQ(px[0 * 6 + 0], 0xFFFFFFFF);
	CHECK_EQ(px[1 * 6 + 0], 0xFFFFFFFF);
	CHECK_EQ(px[0 * 6 + 1], 0);
	/* 'ż' starts at x=4, top row only, clipped at width 6 */
	CHECK_EQ(px[0 * 6 + 4], 0xFFFFFFFF);
	CHECK_EQ(px[0 * 6 + 5], 0xFFFFFFFF);
	CHECK_EQ(px[1 * 6 + 4], 0);
	CHECK_EQ(px[2 * 6 + 0], 0);

	/* negative position and off-canvas rows are clipped, not crashed */
	text_draw(&c, &tfont, -2, 2, "??", -1, 0x11);
	CHECK_EQ(px[2 * 6 + 0], 0x11);
	CHECK_EQ(px[2 * 6 + 5], 0x11);

	canvas_fill(&c, 4, 1, 10, 10, 0x22);
	CHECK_EQ(px[1 * 6 + 4], 0x22);
	CHECK_EQ(px[2 * 6 + 5], 0x22);
	CHECK_EQ(px[1 * 6 + 3], 0);
}

int main(void)
{
	test_utf8();
	test_glyph();
	test_wrap();
	test_draw();
	CHECK_DONE();
}
