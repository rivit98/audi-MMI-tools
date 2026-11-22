/*
 * hbdsi_wire.c - see hbdsi_wire.h.
 */
#include "types.h"
#include <stdlib.h>
#include <string.h>

#include "hbdsi_wire.h"

int hbdsi_attach_msg_init(struct hbdsi_attach_msg *m, const char *name,
                          u16 major, u16 minor)
{
	usize n = strlen(name);

	memset(m, 0, sizeof(*m));
	if (n >= sizeof(m->name))
		return -1;
	m->zero = 0;
	m->one = 1;
	m->major = major;
	m->minor = minor;
	memcpy(m->name, name, n);
	return 0;
}

void hbdsi_hdr_init(struct hbdsi_hdr *h, const struct hbdsi_attach_reply *r,
                    u32 cmd, u32 payload_len)
{
	memset(h, 0, sizeof(*h));
	h->type = HBDSI_MSG_TYPE;
	h->zero = 0;
	h->one = 1;
	h->server_id[0] = r->server_id[0];
	h->server_id[1] = r->server_id[1];
	h->client_id[0] = r->client_id[0];
	h->client_id[1] = r->client_id[1];
	h->cmd = cmd;
	h->total_len = payload_len;
	h->chunk_len = payload_len;
}

int hbdsi_build_request(u8 *buf, usize size, const struct hbdsi_attach_reply *r,
                        u32 request_id, const i32 *args, unsigned nargs)
{
	usize payload = sizeof(struct hbdsi_request_base) + nargs * sizeof(i32);
	struct hbdsi_request_base b;
	struct hbdsi_hdr h;

	if (size < sizeof(h) + payload)
		return -1;
	hbdsi_hdr_init(&h, r, HBDSI_CMD_DATA_REQUEST, (u32)payload);
	memset(&b, 0, sizeof(b));
	b.kind = HBDSI_EVENT_REQUEST;
	b.request_id = request_id;
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), &b, sizeof(b));
	if (nargs)
		memcpy(buf + sizeof(h) + sizeof(b), args, nargs * sizeof(i32));
	return (int)(sizeof(h) + payload);
}

int hbdsi_build_connect(u8 *buf, usize size, const struct hbdsi_attach_reply *r,
                        const struct hbdsi_connect *mine)
{
	struct hbdsi_hdr h;

	if (size < sizeof(h) + sizeof(*mine))
		return -1;
	hbdsi_hdr_init(&h, r, HBDSI_CMD_CONNECT, sizeof(*mine));
	memcpy(buf, &h, sizeof(h));
	memcpy(buf + sizeof(h), mine, sizeof(*mine));
	return (int)(sizeof(h) + sizeof(*mine));
}

/* 1 if want (wl bytes) occurs in s (len bytes). QNX 6.3's libc has no
 * memmem. */
static int contains(const char *s, usize len, const char *want, usize wl)
{
	usize i;

	for (i = 0; i + wl <= len; i++)
		if (memcmp(s + i, want, wl) == 0)
			return 1;
	return 0;
}

/* "1.2" -> 1, 2. 1 if the token is a version. */
static int parse_version(const char *s, usize len, u16 *major, u16 *minor)
{
	u64 a, b;
	char *end;

	if (len == 0 || s[0] < '0' || s[0] > '9')
		return 0;
	a = strtoull(s, &end, 10);
	if (end >= s + len || *end != '.')
		return 0;
	b = strtoull(end + 1, &end, 10);
	if (end != s + len || a > 0xffff || b > 0xffff)
		return 0;
	*major = (u16)a;
	*minor = (u16)b;
	return 1;
}

int hbdsi_listing_find(const char *text, const char *want, unsigned n,
                       char *name, usize name_size, u16 *major, u16 *minor)
{
	const char *line = text;

	while (*line != '\0') {
		const char *eol = strchr(line, '\n'), *p, *tok[8];
		usize len = eol ? (usize)(eol - line) : strlen(line), tlen[8];
		unsigned ntok = 0, i;

		/* split the line into whitespace-separated tokens */
		for (p = line; p < line + len && ntok < 8; ) {
			while (p < line + len && (*p == ' ' || *p == '\t'))
				p++;
			if (p >= line + len)
				break;
			tok[ntok] = p;
			while (p < line + len && *p != ' ' && *p != '\t')
				p++;
			tlen[ntok] = (usize)(p - tok[ntok]);
			ntok++;
		}
		/* a server line ends in a version; the name is the token before */
		if (ntok >= 2 && parse_version(tok[ntok - 1], tlen[ntok - 1], major, minor)) {
			for (i = 0; i + 1 < ntok; i++) {
				usize wl = strlen(want);

				if (tlen[i] < wl || tlen[i] >= name_size)
					continue;
				if (!contains(tok[i], tlen[i], want, wl))
					continue;
				if (n-- > 0)
					break;
				memcpy(name, tok[i], tlen[i]);
				name[tlen[i]] = '\0';
				return 1;
			}
		}
		if (eol == NULL)
			break;
		line = eol + 1;
	}
	return 0;
}

const char *hbdsi_broker_status_name(int status)
{
	switch (status) {
	case HBDSI_BROKER_OK:                      return "ok";
	case HBDSI_BROKER_VERSION_MISMATCH:        return "interface version mismatch";
	case HBDSI_BROKER_UNKNOWN_INTERFACE:       return "unknown interface";
	case HBDSI_BROKER_ALREADY_REGISTERED:      return "interface already registered";
	case HBDSI_BROKER_INVALID_SERVER_ID:       return "invalid server ID";
	case HBDSI_BROKER_INVALID_CLIENT_ID:       return "invalid client ID";
	case HBDSI_BROKER_INVALID_NOTIFICATION_ID: return "invalid notification ID";
	}
	return "unknown status";
}
