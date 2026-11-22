/*
 * cam.h - the rear camera's state for C code (cam.cpp over common/camera).
 */
#ifndef HELLO7_CAM_H
#define HELLO7_CAM_H

#ifdef __cplusplus
extern "C" {
#endif

void cam_start(void);
void cam_stop(void);
/* The HMI shows the camera: show nothing. */
int  cam_active(void);
/* Not active, and the HMI has had time to show its next context. */
int  cam_clear(void);

#ifdef __cplusplus
}
#endif

#endif
