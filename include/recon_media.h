/*
 * What kind of file something is, by its name.
 *
 * One table, one fact, nothing behind it. Anything that has to write a
 * `Content-Type` header can include this without acquiring a dependency: it
 * needs no allocation, no filesystem and no network, and it does not read the
 * file it is naming -- only the characters after the last dot in its name.
 */

#ifndef RECON_MEDIA_H
#define RECON_MEDIA_H

/*
 * What kind of file this is, by its name. Never NULL.
 *
 * Shallow on purpose. Naming a type wrongly means whatever receives it offers
 * the wrong program to open it, which is a nuisance; refusing to name one at
 * all means `application/octet-stream`, which every reader handles by offering
 * to save the file -- the right thing to do with something unrecognised.
 *
 * So this covers what somebody actually attaches or uploads and falls back
 * honestly for everything else, rather than pretending to a table of a
 * thousand types that would be wrong in more interesting ways.
 *
 * It is a file of its own, rather than a static helper beside either of its
 * callers, because both of them -- a letter being composed and a form being
 * submitted -- are writing a `Content-Type` header, and two copies of this
 * table would be one fact with two answers. It is a file of its own rather
 * than a few lines added to a larger one because then a program wanting a
 * media type would link whatever else that file needed, which is how a
 * typedef ended up dragging xkbcommon through twenty-four sources. The extension match is case-insensitive: a camera writes
 * `.JPG`, and that is the same kind of file.
 */
const char *recon_media_type(const char *name);

#endif /* RECON_MEDIA_H */
