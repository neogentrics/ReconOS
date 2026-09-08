/*
 * The error string shared between the two halves of this module.
 *
 * Reading an address and fetching a page were one file, so `set_error` was
 * static and there was nothing to declare. They are two files now -- because
 * a URL parser that cannot be tested without a socket, a registry and a
 * Wayland event loop is a URL parser nobody tests -- and both halves still
 * report through `recon_http_last_error`, so there has to be one buffer and
 * one way to write it.
 *
 * Private on purpose: it is in src/ rather than include/, because nothing
 * outside this module has any business setting the error somebody else will
 * read.
 */

#ifndef RECON_HTTP_ERROR_H
#define RECON_HTTP_ERROR_H

void recon_http_set_error(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));

#endif
