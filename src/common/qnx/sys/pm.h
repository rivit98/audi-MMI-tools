/*
 * sys/pm.h - stand-in for QNX's power-manager header, which the sysroot
 * (openqnx public headers) doesn't have. <sys/iomsg.h> includes it for one
 * type, pm_power_attr_t, used only in the _IO_POWER reply (io_power_t), so
 * <sys/iofunc.h> / <sys/resmgr.h> need it to compile. Nothing of ours
 * sends or answers _IO_POWER; the layout follows QNX 6.3's pm.h but isn't
 * checked against the car.
 */
#ifndef __PM_H_INCLUDED
#define __PM_H_INCLUDED

#include <sys/platform.h>

typedef _Uint32t pm_power_mode_t;

typedef struct pm_power_attr {
	pm_power_mode_t new_mode;
	pm_power_mode_t old_mode;
	pm_power_mode_t next_mode;
	_Int16t         nmodes;
	_Int16t         reserved;
} pm_power_attr_t;

#endif
