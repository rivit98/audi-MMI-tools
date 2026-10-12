/*
 * ui_preview.c - render the usb_tx_test screen on the host, as the MMI would
 * show it, for each link state: build/ui_<state>.bmp (make ui-preview).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "img.h"
#include "log.h"
#include "model.h"
#include "ui.h"

#define W 800
#define H 480

static void save(struct canvas *c, const char *name)
{
	static unsigned char rgba[W * H * 4];
	char path[128];
	int x, y;

	for (y = 0; y < H; y++)
		for (x = 0; x < W; x++) {
			uint32_t p = c->px[(H - 1 - y) * c->stride + x];
			unsigned char *o = rgba + (y * W + x) * 4;

			o[0] = (unsigned char)(p >> 16);
			o[1] = (unsigned char)(p >> 8);
			o[2] = (unsigned char)p;
			o[3] = 0xff;
		}
	snprintf(path, sizeof(path), "build/ui_%s.bmp", name);
	if (img_write_bmp_rgba(path, rgba, W, H) != 0)
		perror(path);
	else
		printf("wrote %s\n", path);
}

static void add(unsigned seq, const char *text)
{
	model_add_message(seq, text, (unsigned)strlen(text));
}

int main(void)
{
	static uint32_t px[W * H];
	struct canvas c = { px, W, H, W };
	char longmsg[1200];
	int i;

	log_open("/dev/null");
	model_init();

	model_set_state(LINK_WAITING, "plug the phone into the car's USB port");
	ui_render(&c, "/fs/sd0");
	save(&c, "waiting");

	model_set_state(LINK_SWITCHING, "phone is switching to accessory mode");
	ui_render(&c, "/fs/sd0");
	save(&c, "switching");

	model_set_state(LINK_ACCESSORY, "open usb_tx_test on the phone");
	ui_render(&c, "/fs/sd0");
	save(&c, "accessory");

	model_set_state(LINK_CONNECTED, "usb_tx_test-android/1 Xiaomi Redmi Note 8 Pro");
	add(1, "Hello MMI!");
	add(2, "Zażółć gęślą jaźń - Polish letters, € and Ünïcödé.");
	add(3, "line one\nline two\n\nafter an empty line");
	for (i = 0, longmsg[0] = 0; i < 30; i++)
		strcat(longmsg, "a fairly long message that has to wrap ");
	add(4, longmsg);
	add(5, "emoji fall back to '?': \xF0\x9F\x98\x80 and a\ttab");
	ui_render(&c, "/fs/sd0");
	save(&c, "connected");

	model_set_state(LINK_NO_STACK, "usbd_connect: No such file or directory - retrying");
	ui_render(&c, "/fs/sd0");
	save(&c, "no_stack");
	return 0;
}
