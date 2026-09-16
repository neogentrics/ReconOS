/*
 * Asking for part of a file.
 *
 * `Range: bytes=0-499` and its two other shapes. A client that lost a download
 * half way through asks for the rest; a player seeking through a recording asks
 * for the middle.
 *
 * --- One range only, and that is a decision rather than a shortcut ---
 *
 * The standard allows a list -- `bytes=0-99,200-299,5000-5099` -- answered as a
 * `multipart/byteranges` body with a boundary between each part. This refuses
 * a list and serves the whole file instead, which is explicitly allowed: a
 * server **may** ignore a `Range` it does not wish to honour.
 *
 * It is refused because a list is a request that costs the server far more than
 * it costs the client. A few hundred bytes of header can ask for ten thousand
 * one-byte ranges, each needing its own boundary, its own headers and its own
 * seek -- so a small request produces an enormous response and a great deal of
 * work. That has been used as an amplification attack against more than one
 * well-known server, and the multipart writer it needs is a meaningful amount
 * of code that exists only to serve that shape.
 *
 * A client that genuinely wants several pieces can ask several times, which
 * costs it one request per piece and costs this server nothing unusual.
 *
 * --- Why `If-Range` is not optional ---
 *
 * A client resuming a download sends the range it still needs. If the file
 * changed since the first half was fetched, the two halves are from different
 * files and what lands on disk is neither -- with a 206 beside it saying all is
 * well.
 *
 * `If-Range: <validator>` is the guard: honour the range only if the
 * representation is unchanged, and otherwise send the whole thing. It is cheap
 * here because the validator already exists, and leaving it out would be
 * shipping the silent-corruption case on purpose.
 */

#ifndef RECON_HTTP_RANGE_H
#define RECON_HTTP_RANGE_H

#include <stddef.h>

/* What a `Range` header came to. */
#define HTTP_RANGE_NONE           0	/* no range, or one to be ignored */
#define HTTP_RANGE_OK             1	/* `into` is filled in */
#define HTTP_RANGE_UNSATISFIABLE  2	/* answer 416 and say how long it is */

/* Inclusive, as the wire format is: `bytes=0-0` is one byte. */
struct http_range {
	unsigned long first;
	unsigned long last;
};

/*
 * Read a `Range` header against a representation of `length` bytes.
 *
 * Understands the three forms:
 *
 *   bytes=0-499     the first five hundred bytes
 *   bytes=500-      everything from 500 to the end
 *   bytes=-500      the *last* five hundred bytes
 *
 * **A header this does not understand is `HTTP_RANGE_NONE`, not an error.** The
 * standard is explicit that an unsatisfiable-looking or malformed `Range` is to
 * be ignored, and the client then gets the whole representation -- which is
 * always a correct answer to a `GET`. Refusing would break clients over a
 * header they could have omitted.
 *
 * The one case that is *not* ignored is a range that is syntactically fine and
 * starts past the end of the file. That is a client asking for something that
 * cannot exist, and answering 200 would hand it bytes it did not ask for.
 */
int http_range_parse(const char *header, unsigned long length,
                     struct http_range *into);

/*
 * Should a `Range` be honoured, given an `If-Range` header?
 *
 * Returns 1 when there is no `If-Range` at all, or when it names the validator
 * this representation has. Returns 0 when it names a different one -- and the
 * caller must then send the whole file with a 200, because the client is
 * holding a piece of something else.
 *
 * Only an entity tag is understood. `If-Range` may also carry a date, and this
 * has no clock to compare one against; a date is therefore treated as "not a
 * match", which sends the whole file. That is the safe direction: the failure
 * is a wasted body rather than a corrupt one.
 */
int http_if_range(const char *if_range, const char *etag);

#endif
