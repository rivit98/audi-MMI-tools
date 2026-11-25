// carlog.h - the car's signals while the receiver runs, for measuring on the
// road: night (DA01.0C0D byte 0), exterior light, climate, driving state,
// cluster, from the camera library's ch4 reader (camera::subscribe_ch4; read
// only, never a second reader). common/camera/carsig.h makes the lines.
//
// audi_stream_receiver.log gets "car:" lines: the first value and the changes
// of every non-BAP function (at most one line per function per 5 s, the
// changes in between counted), every night change, and once a minute the
// night state, the changes per function and the ch4 reader's state (a
// stopped reader shows there).
// <var>/audi_stream_receiver_ch4.txt gets every telegram, decoded (the
// previous run's is kept as *.prev.txt).
#ifndef AUDI_STREAM_RECEIVER_CARLOG_H
#define AUDI_STREAM_RECEIVER_CARLOG_H

namespace carlog {

// After camera::start().
void start(const char *sd_path);
// Before camera::stop(): logs what is held back and closes the file.
void stop();

} // namespace carlog

#endif
