/*
 * The two halves of the codec pack, to each other.
 *
 * Sound and pictures are in separate files because they have almost nothing in
 * common beyond the library they borrow: one produces a count of samples, the
 * other produces three planes, a colour space and a moment. Putting them
 * together would mean one file where every function had to say which of the two
 * situations it was in.
 */

#ifndef RECON_CODECPACK_H
#define RECON_CODECPACK_H

#include <stdbool.h>

/* False when H.264 could not be registered, which is what makes the module
 * refuse to load rather than load as something that decodes nothing. */
bool recon_codecpack_video_load(void);
void recon_codecpack_video_unload(void);

#endif /* RECON_CODECPACK_H */
