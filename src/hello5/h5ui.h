/*
 * h5ui.h - hello5's screen, so a phone video of the run shows which chime is
 * sounding. Drawn by the CPU into common/screen.c's layer-manager screen
 * (cid 40). Every screen carries the log clock ("t = ... ms"), so a video
 * frame can be matched to the log line of the same chime.
 *
 * All calls are serialized internally (the chimes are played on their own
 * thread) and do nothing if ui_open() failed. Each returns how long the
 * screen update took in ms, for the log.
 */
#ifndef HELLO5_H5UI_H
#define HELLO5_H5UI_H

int  ui_open(void);
void ui_close(void);

/* Before the first group: "chimes start in <ms>". */
unsigned ui_lead(int ngroups, unsigned ms);

/* Group <group> (= its chime count) on the output <label> ("device 5:
 * tones1", "m2: TDM1 2.0 SAD"); chime <chime> (1-based) is sounding if
 * playing, else it has just ended. */
unsigned ui_chime(int group, int ngroups, const char *label, int chime, int playing);

/* Between groups. next = 0 after the last group. note may be NULL. */
unsigned ui_gap(int done, int next, const char *next_label, const char *note);

/* A listening step (step of nsteps, its colour): text (white) and question
 * (yellow, may be NULL), both word-wrapped, then big in large letters, in
 * a white box if playing, and note (may be NULL) at the bottom. */
unsigned ui_step(int step, int nsteps, const char *title, const char *text,
                 const char *question, const char *big, int playing,
                 const char *note);

#endif
