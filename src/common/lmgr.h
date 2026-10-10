/*
 * lmgr.h - client for the MMI's layer manager (/usr/bin/layermanagerV2,
 * resource manager at /dev/layermanager).
 *
 * Protocol reversed by qnx-audi (from libhybrid.so); command meanings named
 * after layermanagerV2's own log strings. All six devctl codes are present,
 * unchanged, in the P1001 MMI3GApplication / MMI3GMedia / MMI3GNavigation
 * and layermanagerV2 binaries.
 *
 * Displayables are numbered "cids"; the layer manager hands each one a
 * virtual frame buffer (VFB): a gf surface identified by a sid.
 */
#ifndef RIVIT_LMGR_H
#define RIVIT_LMGR_H

/* devctl codes: __DIOT/__DIOTF(class 5, cmd, payload size) */
#define LM_DCMD_REGISTER_DISPLAYABLE 0xC0140500u /* cid,w,h,flags -> ok   */
#define LM_DCMD_501                  0x80040501u /* cid (qnx-audi: "unknown") */
#define LM_DCMD_GET_VFB              0xC0100503u /* cid -> sid,uid,alpha  */
#define LM_DCMD_UPDATE_VFB           0x80040504u /* cid                   */
#define LM_DCMD_SHOW_DISPLAYABLES    0x800C0505u /* display?,n,cids[n]    */
#define LM_DCMD_CHECK_VERSION        0xC00C0506u /* -> major,minor        */

struct lm_vfb {
	unsigned sid;       /* gf surface id of the frame buffer */
	unsigned uid;
	unsigned alpha_sid; /* gf surface id of the alpha plane */
};

/* All return 0 on success or an errno value (devctl/open/write status). */
int  lm_open(void);
void lm_close(void);
int  lm_check_version(int *major, int *minor);
int  lm_register_displayable(int cid, int w, int h, int flags);
int  lm_get_vfb(int cid, struct lm_vfb *vfb);
int  lm_update_vfb(int cid);
int  lm_cmd_501(int cid);
int  lm_show_displayables(int display, const int *cids, int n);

/* Text command interface (write() to /dev/layermanager), e.g.
 *   "s-shot <display> <file>"      screenshot of a whole display
 *   "d-shot <displayable> <file>"  screenshot of one displayable's VFB
 * The file type follows the extension (img_codec_{png,bmp,jpg,gif}). */
int  lm_text_command(const char *cmd);

#endif
