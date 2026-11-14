/*
 * hbdsi_wire.h - the wire format of Harman's MOCCA "HBDSI": how the unit's
 * native processes call each other's service interfaces (MMI3GNavigation
 * asking MMI3GApplication's audio management for the nav voice channel,
 * for example). Pure: no QNX calls, host-tested (tests/test_hbdsi.c).
 * hbdsi.c sends it. Where every field comes from: docs/hbdsi.md.
 *
 * Two layers:
 *  - the servicebroker (/srv/servicebroker, a resource manager). A server
 *    registers an interface by name and version; a client attaches with a
 *    devctl and gets the server's nd/pid/chid and the connection ids back.
 *  - HBDSI messages, QNX MsgSend to the server's channel: a 0x28-byte
 *    header, then a payload. ConnectRequest (9) swaps channel ids;
 *    DataRequest (7) carries one serialized request event.
 */
#ifndef COMMON_HBDSI_WIRE_H
#define COMMON_HBDSI_WIRE_H

#include "types.h"

#define HBDSI_BROKER_PATH "/srv/servicebroker"

/* servicebroker devctls (QNX _DEVCTL_DATA_IO, class 5) */
#define HBDSI_DCMD_REGISTER   0xC1180500u
#define HBDSI_DCMD_UNREGISTER 0xC00C0501u
#define HBDSI_DCMD_ATTACH     0xC1080502u
#define HBDSI_DCMD_DETACH     0xC00C0503u

/* the broker's status, returned as the devctl's dev_info */
enum {
	HBDSI_BROKER_OK = 0,
	HBDSI_BROKER_VERSION_MISMATCH = 1,
	HBDSI_BROKER_UNKNOWN_INTERFACE = 2,
	HBDSI_BROKER_ALREADY_REGISTERED = 3,
	HBDSI_BROKER_INVALID_SERVER_ID = 4,
	HBDSI_BROKER_INVALID_CLIENT_ID = 5,
	HBDSI_BROKER_INVALID_NOTIFICATION_ID = 6,
};

#define HBDSI_NAME_MAX 256

/* DCMD_ATTACH, in (0x108 bytes) */
struct hbdsi_attach_msg {
	u16  zero;              /* 0 */
	u16  one;               /* 1: the broker refuses anything above */
	u16  major, minor;      /* interface version */
	char name[HBDSI_NAME_MAX];
};

/* DCMD_ATTACH, out (0x20 bytes) */
struct hbdsi_attach_reply {
	u32 word0;              /* a server field (+0x20 of its record) */
	u32 nd;                 /* the server's node, as seen from us */
	u32 pid;                /* the server process */
	u32 chid;               /* its receive channel */
	u32 server_id[2];       /* header +0x08 of every message */
	u32 client_id[2];       /* header +0x10: [0] = our client number */
};

/* HBDSI message commands (header +0x18) */
enum {
	HBDSI_CMD_QUIT = 6,
	HBDSI_CMD_DATA_REQUEST = 7,
	HBDSI_CMD_DATA_RESPONSE = 8,
	HBDSI_CMD_CONNECT = 9,
	HBDSI_CMD_DISCONNECT = 10,
	HBDSI_CMD_INTERNAL = 15,
};

#define HBDSI_MSG_TYPE 0x200u   /* first QNX message type above _IO_MAX */

struct hbdsi_hdr {
	u32 type;               /* HBDSI_MSG_TYPE */
	u16 zero;               /* 0 */
	u16 one;                /* 1 */
	u32 server_id[2];       /* from the attach reply */
	u32 client_id[2];
	u32 cmd;                /* HBDSI_CMD_* */
	u32 total_len;          /* payload bytes of the whole message */
	u32 chunk_len;          /* payload bytes in this one */
	u32 reserved;           /* 0 */
};

/* ConnectRequest payload, and the server's 16-byte reply: each side's two
 * receive channels, its pid and node. */
struct hbdsi_connect {
	u32 chid[2];
	u32 pid;
	u32 nd;
};

/* A serialized request event: the event's 16-byte base, then its
 * arguments. The receiver checks kind (payload +4 = message +0x2c). */
#define HBDSI_EVENT_REQUEST 0x100u
struct hbdsi_request_base {
	u32 zero0;
	u32 kind;               /* HBDSI_EVENT_REQUEST */
	u32 request_id;         /* the interface's method number */
	u32 zero1;
};

_Static_assert(sizeof(struct hbdsi_attach_msg) == 0x108, "attach msg");
_Static_assert(sizeof(struct hbdsi_attach_reply) == 0x20, "attach reply");
_Static_assert(sizeof(struct hbdsi_hdr) == 0x28, "hbdsi header");
_Static_assert(sizeof(struct hbdsi_connect) == 0x10, "connect payload");
_Static_assert(sizeof(struct hbdsi_request_base) == 0x10, "request base");
_Static_assert(__builtin_offsetof(struct hbdsi_hdr, cmd) == 0x18, "cmd at 0x18");
_Static_assert(__builtin_offsetof(struct hbdsi_hdr, total_len) == 0x1c, "len at 0x1c");

/* SGAudioManagement, served by MMI3GApplication's AudioManagementDevCtrl:
 * request ids (the stub's dispatch) and the arguments used here. */
enum {
	SGAM_REQ_AUDIO_FOCUS = 2,       /* RequestAudioFocus(src, sink) */
	SGAM_REQ_RELEASE_FOCUS = 3,     /* ReleaseAudioFocus(src, sink) */
	SGAM_REQ_MAKE_AUDIBLE = 4,      /* MakeAudioSourceAudible(src, sink) */
};
#define SGAM_SRC_NAVTTS   55    /* eSRC_NAVTTS: logical source 0xe */
#define SGAM_SNK_SPEAKERS 0     /* eSNK_SPEAKERS */

/* Fill an attach message. -1 if the name does not fit. */
int hbdsi_attach_msg_init(struct hbdsi_attach_msg *m, const char *name,
                          u16 major, u16 minor);

/* A message header from the attach reply's ids. */
void hbdsi_hdr_init(struct hbdsi_hdr *h, const struct hbdsi_attach_reply *r,
                    u32 cmd, u32 payload_len);

/* A whole DataRequest (header + request base + n int32 arguments) into
 * buf. Its length, or -1 if size is too small. */
int hbdsi_build_request(u8 *buf, usize size, const struct hbdsi_attach_reply *r,
                        u32 request_id, const i32 *args, unsigned nargs);

/* A ConnectRequest (header + struct hbdsi_connect) into buf. Its length,
 * or -1 if size is too small. */
int hbdsi_build_connect(u8 *buf, usize size, const struct hbdsi_attach_reply *r,
                        const struct hbdsi_connect *mine);

/* The servicebroker's status text (read from /srv/servicebroker): find
 * the n-th (0-based) line naming an interface that contains want; its
 * name into name, its version into *major and *minor. Lines look like
 * " <id>  <nd/pid/chid>  <name>  <major>.<minor>". 1 found, 0 not. */
int hbdsi_listing_find(const char *text, const char *want, unsigned n,
                       char *name, usize name_size, u16 *major, u16 *minor);

/* The broker status as text. */
const char *hbdsi_broker_status_name(int status);

#endif
