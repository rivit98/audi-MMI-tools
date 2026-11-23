// cam.cpp - common/camera (C++) for hello7's C code: is the rear camera up
// (the slog and its BAP session on ch4, docs/rear-camera.md §9.1)?
#include "cam.h"

#include "camera.h"

extern "C" {

void cam_start(void)
{
	camera::start();
}

void cam_stop(void)
{
	camera::stop();
}

int cam_active(void)
{
	return camera::active();
}

int cam_clear(void)
{
	return camera::clear();
}

}
