/*
 * aoa.h - USB side of usb_tx_test: an Android Open Accessory host on the
 * MMI's io-usb stack (libusbdi).
 *
 * A manager thread owns everything:
 *   1. usbd_connect() to io-usb with an all-wildcard filter (retried every
 *      2 s while io-usb is not there).
 *   2. Any new device, 1 s after it appears (the media launcher probes it
 *      first): AOA handshake on its control pipe (GET_PROTOCOL 51, six
 *      identification strings 52, START 53). A device that refuses
 *      GET_PROTOCOL (a USB stick, a hub, an iPod) is left alone; any other
 *      failure is retried up to 5 times with a growing delay.
 *   3. The phone re-enumerates as 18d1:2d00..2d05. Its accessory interface
 *      (class/subclass ff/ff) gets a bulk IN and OUT pipe; IN is read
 *      continuously with asynchronous usbd_io(), frames go to model.h
 *      (HELLO: the peer) and, reassembled by vasm.h, video.h (pictures). The
 *      MMI's VIDEO_CAPS goes out with its HELLO; VIDEO_ACKs queued by
 *      video.c are sent from the manager thread.
 *   4. Removal or any transfer error tears the connection down and goes
 *      back to waiting. Nothing in here ever exits the process.
 *
 * Callbacks from io-usb only queue events; all blocking work runs on the
 * manager thread.
 */
#ifndef USBTX_AOA_H
#define USBTX_AOA_H

/* Identification sent to the phone; the Android app's accessory_filter.xml
 * matches manufacturer + model. */
#define AOA_MANUFACTURER "mmi3g"
#define AOA_MODEL        "usb_tx_test"
#define AOA_DESCRIPTION  "MMI 3G+ usb_tx_test receiver"
#define AOA_VERSION      "1"
#define AOA_URI          "https://github.com/dspl1236/MMI3G-Toolkit"
#define AOA_SERIAL       "mmi3g-0001"

#define USBTX_HELLO      "usb_tx_test-mmi/1"

/* System log snapshots per run: <sd>/var/usb_slog_1.txt ... _<this>.txt */
#define AOA_SLOG_SNAPSHOTS 6

/* 0, or -1 if the manager thread could not start. sd_path: where a failed
 * handshake or a removal saves a system log snapshot (var/usb_slog_<n>.txt),
 * NULL for none. */
int  aoa_start(const char *sd_path);
void aoa_stop(void);    /* tear down and join; bounded by quit.c's hard alarm */

#endif
