/*
 * sys/usbdi.h - minimal QNX USB host client (USBDI) declarations for the
 * MMI 3G+ SDK.
 *
 * openqnx (6.4.1) does not ship the USB DDK headers, so this is a
 * reconstruction: just the pieces usb_tx_test needs to talk to a phone
 * through io-usb (/dev/io-usb/io-usb) with the device's own libusbdi.so.2
 * (/mnt/efs-system/lib, "632-2077" build of io-usb).
 *
 * Names, constants and struct layouts follow the QNX 6.5.0 SDK's
 * sys/usbdi.h + sys/usb100.h, which is the same ABI generation. Checked
 * against the device's libusbdi.so.2 (SH4 disassembly, file offsets):
 *
 *   usbd_connect @0x8180 reads parm->vusbd @+6 and only accepts 0 or 0x0101
 *       (else returns 0x113), flags @+8, argc @+12, argv @+16, evtbufsz
 *       @+20 (default 1000 events), ident @+24, funcs @+28, connect_wait
 *       (u16) @+32, evtprio (u8) @+34.                          [CONFIRMED]
 *   usbd_setup_vendor @0x8d60 (urb, flags, request, rtype, value, index,
 *       addr, len): flags kept in the low 16 bits; unless bit 0x20000 is
 *       set it translates addr with usbd_mphys(), so addr MUST come from
 *       usbd_alloc().                                           [CONFIRMED]
 *   usbd_setup_bulk @0x8f80 (urb, flags, addr, len): same flag/address
 *       handling.                                               [CONFIRMED]
 *   URB_DIR_* / USBD_STATUS_* / USB_* values: from the 6.5.0 header and
 *       smartdevicelink's QNX transport (which shipped on QNX head units).
 *                                                               [INFERRED]
 *
 * Threads: usbd_connect() starts a thread that calls insertion/removal and
 * the completion callbacks of asynchronous usbd_io(). Don't block in them.
 */
#ifndef _SYS_USBDI_H_INCLUDED
#define _SYS_USBDI_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

/* --- sys/usb100.h --------------------------------------------------- */

#define USB_VERSION                 0x0110

#define USB_DESC_DEVICE             0x01
#define USB_DESC_CONFIGURATION      0x02
#define USB_DESC_STRING             0x03
#define USB_DESC_INTERFACE          0x04
#define USB_DESC_ENDPOINT           0x05

#define USB_ENDPOINT_MASK           0x0F
#define USB_ENDPOINT_IN             0x80
#define USB_ENDPOINT_OUT            0x00

#define USB_ATTRIB_CONTROL          0x00
#define USB_ATTRIB_ISOCHRONOUS      0x01
#define USB_ATTRIB_BULK             0x02
#define USB_ATTRIB_INTERRUPT        0x03

/* bmRequestType parts (the rtype argument of usbd_setup_vendor) */
#define USB_RECIPIENT_DEVICE        (0 << 0)
#define USB_RECIPIENT_INTERFACE     (1 << 0)
#define USB_RECIPIENT_ENDPOINT      (2 << 0)
#define USB_RECIPIENT_OTHER         (3 << 0)
#define USB_TYPE_STANDARD           (0 << 5)
#define USB_TYPE_CLASS              (1 << 5)
#define USB_TYPE_VENDOR             (2 << 5)
#define USB_DIRECTION_DEVICE        (0 << 7)
#define USB_DIRECTION_HOST          (1 << 7)

#define USB_CLASS_HUB               0x09

typedef struct _usb100_generic_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint8_t  bData[1];
} usb100_generic_descriptor_t;

/* --- sys/usbdi.h ---------------------------------------------------- */

#define USBD_VERSION                0x0101   /* usbd_connect accepts 0 or this */
#define USBD_CONNECT_WAIT           60
#define USBD_CONNECT_WILDCARD       0xffffffffu

#define URB_DIR_IN                  0x00000001
#define URB_DIR_OUT                 0x00000002
#define URB_DIR_NONE                0x00000003
#define URB_SHORT_XFER_OK           0x00000004

#define USBD_URB_STATUS_MASK        0xFF000000u
#define USBD_STATUS_INPROG          0x00000000u
#define USBD_STATUS_CMP             0x01000000u
#define USBD_STATUS_CMP_ERR         0x02000000u
#define USBD_STATUS_TIMEOUT         0x04000000u
#define USBD_STATUS_ABORTED         0x08000000u
#define USBD_STATUS_DEVICE_RESET    0x10000000u
#define USBD_USB_STATUS_MASK        0x00FFFFFFu
#define USBD_STATUS_STALL           0x00000004u
#define USBD_STATUS_DEV_NOANSWER    0x00000005u
#define USBD_STATUS_DATA_UNDERRUN   0x00000009u

#define USBD_TIME_DEFAULT           0x00000000u
#define USBD_TIME_INFINITY          0xFFFFFFFFu

struct usbd_connection;
struct usbd_desc_node;
struct usbd_device;
struct usbd_pipe;
struct usbd_urb;

typedef struct usbd_device_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint16_t bcdUSB;
	uint8_t  bDeviceClass;
	uint8_t  bDeviceSubClass;
	uint8_t  bDeviceProtocol;
	uint8_t  bMaxPacketSize0;
	uint16_t idVendor;
	uint16_t idProduct;
	uint16_t bcdDevice;
	uint8_t  iManufacturer;
	uint8_t  iProduct;
	uint8_t  iSerialNumber;
	uint8_t  bNumConfigurations;
} usbd_device_descriptor_t;

typedef struct usbd_configuration_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint16_t wTotalLength;
	uint8_t  bNumInterfaces;
	uint8_t  bConfigurationValue;
	uint8_t  iConfiguration;
	uint8_t  bmAttributes;
	uint8_t  MaxPower;
} usbd_configuration_descriptor_t;

typedef struct usbd_interface_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint8_t  bInterfaceNumber;
	uint8_t  bAlternateSetting;
	uint8_t  bNumEndpoints;
	uint8_t  bInterfaceClass;
	uint8_t  bInterfaceSubClass;
	uint8_t  bInterfaceProtocol;
	uint8_t  iInterface;
} usbd_interface_descriptor_t;

typedef struct usbd_endpoint_descriptor {
	uint8_t  bLength;
	uint8_t  bDescriptorType;
	uint8_t  bEndpointAddress;
	uint8_t  bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t  bInterval;
} usbd_endpoint_descriptor_t;

typedef union usbd_descriptors {
	usb100_generic_descriptor_t     generic;
	usbd_device_descriptor_t        device;
	usbd_configuration_descriptor_t configuration;
	usbd_interface_descriptor_t     interface;
	usbd_endpoint_descriptor_t      endpoint;
} usbd_descriptors_t;

typedef struct usbd_device_ident {
	uint32_t vendor;
	uint32_t device;
	uint32_t dclass;
	uint32_t subclass;
	uint32_t protocol;
} usbd_device_ident_t;

/* What an insertion/removal callback gets. One callback per matching
 * interface: a multi-interface device arrives more than once with the same
 * path/devno/generation. */
typedef struct usbd_device_instance {
	uint8_t             path;
	uint8_t             devno;
	uint16_t            generation;
	usbd_device_ident_t ident;
	uint32_t            config;
	uint32_t            iface;
	uint32_t            alternate;
} usbd_device_instance_t;

typedef struct usbd_funcs {
	uint32_t nentries;
	void (*insertion)(struct usbd_connection *, usbd_device_instance_t *instance);
	void (*removal)(struct usbd_connection *, usbd_device_instance_t *instance);
	void (*event)(struct usbd_connection *, usbd_device_instance_t *instance, uint16_t type);
} usbd_funcs_t;

#define _USBDI_NFUNCS ((sizeof(usbd_funcs_t) - sizeof(uint32_t)) / sizeof(void *))

typedef struct usbd_connect_parm {
	const char          *path;         /* NULL: /dev/io-usb/io-usb */
	uint16_t             vusb;         /* USB_VERSION */
	uint16_t             vusbd;        /* USBD_VERSION */
	uint32_t             flags;
	int32_t              argc;
	char               **argv;
	uint32_t             evtbufsz;     /* 0: default */
	usbd_device_ident_t *ident;
	usbd_funcs_t        *funcs;
	uint16_t             connect_wait; /* seconds, or USBD_CONNECT_WAIT */
	uint8_t              evtprio;
	uint8_t              _reserved[13];
} usbd_connect_parm_t;

_Static_assert(sizeof(usbd_device_descriptor_t) == 18, "device descriptor");
_Static_assert(offsetof(usbd_endpoint_descriptor_t, wMaxPacketSize) == 4, "endpoint");
_Static_assert(sizeof(usbd_device_instance_t) == 36, "device instance");
_Static_assert(offsetof(usbd_connect_parm_t, vusbd) == 6, "vusbd@6");
_Static_assert(offsetof(usbd_connect_parm_t, flags) == 8, "flags@8");
_Static_assert(offsetof(usbd_connect_parm_t, evtbufsz) == 20, "evtbufsz@20");
_Static_assert(offsetof(usbd_connect_parm_t, ident) == 24, "ident@24");
_Static_assert(offsetof(usbd_connect_parm_t, funcs) == 28, "funcs@28");
_Static_assert(offsetof(usbd_connect_parm_t, connect_wait) == 32, "connect_wait@32");
_Static_assert(offsetof(usbd_connect_parm_t, evtprio) == 34, "evtprio@34");

#ifdef __cplusplus
extern "C" {
#endif

int  usbd_connect(usbd_connect_parm_t *parm, struct usbd_connection **connection);
int  usbd_disconnect(struct usbd_connection *connection);

int  usbd_attach(struct usbd_connection *connection, usbd_device_instance_t *instance,
                 size_t extra, struct usbd_device **device);
int  usbd_detach(struct usbd_device *device);
int  usbd_select_config(struct usbd_device *device, uint8_t config);

usbd_device_descriptor_t *usbd_device_descriptor(struct usbd_device *device,
                                                 struct usbd_desc_node **node);
/* Walk the descriptor tree: the index-th descriptor of `type` below `root`
 * (NULL = device). *node (may be NULL) is the found node, for walking on. */
usbd_descriptors_t *usbd_parse_descriptors(struct usbd_device *device,
                                           struct usbd_desc_node *root, uint8_t type,
                                           int index, struct usbd_desc_node **node);

/* desc = the device descriptor gives the default control pipe. */
int  usbd_open_pipe(struct usbd_device *device, usbd_descriptors_t *desc,
                    struct usbd_pipe **pipe);
int  usbd_close_pipe(struct usbd_pipe *pipe);
int  usbd_abort_pipe(struct usbd_pipe *pipe);
int  usbd_reset_pipe(struct usbd_pipe *pipe);

struct usbd_urb *usbd_alloc_urb(struct usbd_urb *link);
struct usbd_urb *usbd_free_urb(struct usbd_urb *urb);
/* DMA-able memory: every transfer buffer must come from here. */
void *usbd_alloc(size_t size);
void  usbd_free(void *ptr);

int  usbd_setup_vendor(struct usbd_urb *urb, uint32_t flags, uint16_t request,
                       uint16_t rtype, uint16_t value, uint16_t index,
                       void *addr, uint32_t len);
int  usbd_setup_bulk(struct usbd_urb *urb, uint32_t flags, void *addr, uint32_t len);

/* func == NULL: synchronous, returns when the transfer is done (or the
 * timeout in ms expires). Otherwise asynchronous: func runs on the usbd
 * thread when it completes. */
int  usbd_io(struct usbd_urb *urb, struct usbd_pipe *pipe,
             void (*func)(struct usbd_urb *, struct usbd_pipe *, void *),
             void *handle, uint32_t timeout);
int  usbd_urb_status(struct usbd_urb *urb, uint32_t *status, uint32_t *length);

#ifdef __cplusplus
}
#endif

#endif
