/*
 * Validators, and asking a client whether it already has this.
 *
 * --- Why an ETag and not `Last-Modified` ---
 *
 * `Last-Modified` is the cheaper validator everywhere else, and it is not
 * available here: this C library has no `stat`, so a file's modification time
 * cannot be asked for. That is the whole reason this file exists in the shape
 * it does.
 *
 * What is left is the content. So the validator is a hash of the bytes, which
 * is a **strong** validator -- it changes when the content changes and not
 * when anything else does. A file touched without being edited keeps its tag,
 * which `Last-Modified` gets wrong in the expensive direction.
 *
 * --- What that costs, stated rather than hidden ---
 *
 * The tag has to be in the head, and the head goes out before the body. So the
 * file is read **twice**: once to hash, once to send. For a console's assets
 * that is the right trade -- a 304 saves a whole body -- and for a large file
 * served once it is a real cost. `docs/WEB.md` carries the row.
 *
 * --- The hazard this creates, and what is done about it ---
 *
 * Hashing and sending are two observations of a file that something else may
 * change in between. A wrong ETag is worse than a wrong length: a length
 * mismatch breaks one connection, while a tag that does not match its bytes is
 * cached by the client and served from that cache **until it expires**. One
 * bad answer becomes every answer.
 *
 * So the bytes are hashed again on the way out and compared. They cannot be
 * un-sent -- the head is long gone by then -- but the connection can be closed
 * rather than kept, which is the same treatment a broken length promise gets
 * and for the same reason: the failure is detectable, so it is not silent.
 */

#ifndef RECON_HTTP_CACHE_H
#define RECON_HTTP_CACHE_H

#include <stddef.h>

/* Room for a formatted tag, quotes and terminator included. */
#define HTTP_ETAG_MAX 40

/*
 * FNV-1a, 64-bit.
 *
 * **Not a cryptographic hash and not used as one.** An ETag answers "is this
 * the same bytes I had before", asked by a client about its own cache. It is
 * not a signature and nothing trusts it across a boundary, so collision
 * resistance against an adversary is not the property wanted -- speed over a
 * whole file on a machine with no hardware to help is.
 *
 * Chosen over a CRC because a CRC's collisions are *structured*: two files
 * differing by a short run of bytes can share one, and files on a volume tend
 * to differ by short runs. FNV spreads them.
 *
 * If a validator ever needs to be trusted rather than merely compared, this is
 * the wrong function and the comment above it should be read first.
 */
#define HTTP_HASH_SEED 0xcbf29ce484222325ULL

unsigned long long http_hash(unsigned long long state, const void *data,
                             size_t len);

/* Format a strong validator from a file's length and its hash.
 *
 * Both, rather than the hash alone: two files of different lengths that
 * happened to hash alike would otherwise share a tag, and length is free.
 * Written with the quotes, because an ETag without them is not one. */
void http_etag_format(char *into, size_t room, unsigned long length,
                      unsigned long long hash);

/*
 * Does an `If-None-Match` header match this tag?
 *
 * Handles `*`, a single tag, and a comma-separated list. A weak tag (`W/"x"`)
 * matches its strong form, which is what the weak comparison the standard
 * specifies for `If-None-Match` requires -- and is the opposite of what
 * `If-Match` would want, which is why this function names the header it is
 * for rather than being called `etag_matches`.
 *
 * Returns 1 on a match, 0 otherwise. A malformed header is not a match and is
 * not an error: the client gets the body it would have got anyway.
 */
int http_if_none_match(const char *header, const char *etag);

#endif
