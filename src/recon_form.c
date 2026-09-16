/*
 * What a form sends. See include/recon_form.h.
 *
 * Nothing here opens anything. It reads a parsed document and a table of live
 * values and produces the bytes of a request, which is why it is its own file:
 * the code that decides what leaves this machine should be testable without a
 * compositor under it.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recon_form.h"
#include "recon_media.h"

/* One byte, encoded. Returns what it wrote, or 0 when it would not fit. */
static size_t encode_byte(unsigned char c, char *out, size_t size) {
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
            c == '~') {
        if (size < 2) {
            return 0;
        }
        out[0] = (char)c;
        out[1] = '\0';
        return 1;
    }
    if (c == ' ') {
        if (size < 2) {
            return 0;
        }
        out[0] = '+';
        out[1] = '\0';
        return 1;
    }
    if (size < 4) {
        return 0;
    }
    static const char HEX[] = "0123456789ABCDEF";
    out[0] = '%';
    out[1] = HEX[(c >> 4) & 0xF];
    out[2] = HEX[c & 0xF];
    out[3] = '\0';
    return 3;
}

size_t recon_form_encode(const char *text, char *out, size_t size) {
    if (out == NULL || size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (text == NULL) {
        return 0;
    }

    size_t used = 0;
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0';
            c++) {
        char piece[8];
        size_t took = encode_byte(*c, piece, sizeof(piece));
        if (took == 0 || used + took + 1 > size) {
            out[0] = '\0';
            return 0;
        }
        memcpy(out + used, piece, took);
        used += took;
        out[used] = '\0';
    }
    return used;
}

/* Append encoded text to a growing buffer. False when it would not fit. */
static bool append_encoded(char *out, size_t size, size_t *used,
        const char *text) {
    for (const unsigned char *c = (const unsigned char *)text; *c != '\0';
            c++) {
        char piece[8];
        size_t took = encode_byte(*c, piece, sizeof(piece));
        if (took == 0 || *used + took + 1 >= size) {
            return false;
        }
        memcpy(out + *used, piece, took);
        *used += took;
        out[*used] = '\0';
    }
    return true;
}

const char *recon_form_field_sends(const struct recon_html_document *page,
        int index, const struct recon_form_value *values, int value_count,
        int submitter) {
    const struct recon_html_field *d = recon_html_field_at(page, index);
    if (d == NULL || values == NULL || index < 0 || index >= value_count) {
        return NULL;
    }
    if (d->name[0] == '\0' || d->disabled) {
        return NULL;
    }
    const struct recon_form_value *live = &values[index];
    const char *text = live->text != NULL ? live->text : "";

    switch (d->kind) {
    case RECON_HTML_FIELD_CHECKBOX:
    case RECON_HTML_FIELD_RADIO:
        if (!live->on) {
            return NULL;
        }
        return text[0] != '\0' ? text : "on";

    case RECON_HTML_FIELD_SUBMIT:
        return (index == submitter) ? text : NULL;

    case RECON_HTML_FIELD_RESET:
    case RECON_HTML_FIELD_BUTTON:
        return NULL;

    case RECON_HTML_FIELD_FILE:
        /*
         * An empty value, and it is not a stand-in.
         *
         * A browser with no file chosen sends exactly this for a form that
         * does not ask for multipart -- the name, and nothing after the
         * equals sign. This viewer has no file chosen either, so the two
         * requests are the same request.
         *
         * A form that *does* ask for multipart never gets here: `recon_web.c`
         * refuses it before a body is built, because a url-encoded body is
         * not a degraded multipart one, it is an unintelligible one.
         */
        return "";

    case RECON_HTML_FIELD_CHOICE: {
        /*
         * A menu with nothing chosen sends nothing at all rather than an
         * empty value. That happens only for a `<select>` with no options in
         * it, which is a page bug -- and inventing an empty answer for it
         * would put a parameter the page never described into the request.
         */
        if (d->option_count <= 0 || live->chosen < 0 ||
                live->chosen >= d->option_count) {
            return NULL;
        }
        const struct recon_html_option *opt =
            recon_html_option_at(page, d->first_option + live->chosen);
        return opt != NULL ? opt->value : NULL;
    }

    default:
        return text;
    }
}

char *recon_form_body(const struct recon_html_document *page, int form,
        int submitter, const struct recon_form_value *values, int value_count,
        size_t limit, int *out_count, bool *out_secret) {
    if (out_count != NULL) {
        *out_count = 0;
    }
    if (out_secret != NULL) {
        *out_secret = false;
    }
    if (page == NULL || values == NULL || limit < 2) {
        return NULL;
    }

    char *body = calloc(1, limit);
    if (body == NULL) {
        return NULL;
    }
    size_t used = 0;
    int count = 0;
    bool secret = false;

    int fields = recon_html_field_count(page);
    for (int i = 0; i < fields && i < value_count; i++) {
        const struct recon_html_field *d = recon_html_field_at(page, i);
        if (d == NULL || d->form != form) {
            continue;
        }
        const char *value = recon_form_field_sends(page, i, values,
            value_count, submitter);
        if (value == NULL) {
            continue;
        }

        if (used > 0) {
            if (used + 2 >= limit) {
                free(body);
                return NULL;
            }
            body[used++] = '&';
            body[used] = '\0';
        }
        if (!append_encoded(body, limit, &used, d->name)) {
            free(body);
            return NULL;
        }
        if (used + 2 >= limit) {
            free(body);
            return NULL;
        }
        body[used++] = '=';
        body[used] = '\0';
        if (!append_encoded(body, limit, &used, value)) {
            free(body);
            return NULL;
        }

        count++;
        if (d->secret) {
            secret = true;
        }
    }

    if (out_count != NULL) {
        *out_count = count;
    }
    if (out_secret != NULL) {
        *out_secret = secret;
    }
    return body;
}

/* --- multipart/form-data ---------------------------------------------- */

/*
 * Append bytes, keeping the buffer NUL-terminated on top of its real length.
 *
 * False when they will not fit, and then the caller frees and refuses. There
 * is deliberately no "write what fits" path: a request cut in the middle of a
 * part is one a server parses successfully and misreads.
 */
static bool put_bytes(char *out, size_t size, size_t *used,
        const char *text, size_t n) {
    if (n + 1 > size - *used) {
        return false;
    }
    memcpy(out + *used, text, n);
    *used += n;
    out[*used] = '\0';
    return true;
}

static bool put(char *out, size_t size, size_t *used, const char *text) {
    return put_bytes(out, size, used, text, strlen(text));
}

/*
 * A name or a filename as it appears inside quotes in a part header.
 *
 * Three bytes are escaped and no others: a carriage return, a newline and a
 * double quote. That is not a simplification -- it is exactly what the HTML
 * standard asks for, and the reason is that those three are the only ones that
 * could end the header early or end the quoted string early. A percent sign is
 * left alone, which looks like an oversight and is not: the receiver does not
 * percent-decode these, so escaping one would change the name.
 *
 * Without this, a field named `a"; filename="b` writes a second attribute into
 * the header, and a page gets to say things about the upload that the person
 * uploading never agreed to.
 */
static bool put_quoted(char *out, size_t size, size_t *used, const char *text) {
    for (const char *p = text; *p != '\0'; p++) {
        const char *escape = NULL;
        switch (*p) {
        case '\r': escape = "%0D"; break;
        case '\n': escape = "%0A"; break;
        case '"':  escape = "%22"; break;
        default:   break;
        }
        bool ok = (escape != NULL)
            ? put(out, size, used, escape)
            : put_bytes(out, size, used, p, 1);
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* Whether `haystack` contains `needle`, over bytes rather than a C string, so
 * a file with a zero byte in the middle of it is still searched to the end. */
static bool bytes_contain(const char *haystack, size_t size,
        const char *needle) {
    size_t n = strlen(needle);
    if (n == 0 || size < n) {
        return false;
    }
    for (size_t i = 0; i + n <= size; i++) {
        if (memcmp(haystack + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

/*
 * What this field contributes to a multipart body: a value, or a file, or
 * nothing at all.
 *
 * The question of *whether* it is sent is not re-answered here.
 * `recon_form_field_sends` owns that -- unnamed, disabled, an unticked box, a
 * submit button that was not the one pressed -- and this asks it and then only
 * decides what the bytes are. Two places deciding what a form sends is how a
 * checkbox ends up in one encoding and not the other.
 */
static bool part_of(const struct recon_html_document *page, int index,
        const struct recon_form_value *values, int value_count, int submitter,
        const char **out_text, size_t *out_length, const char **out_filename) {
    const char *sends = recon_form_field_sends(page, index, values,
        value_count, submitter);
    if (sends == NULL) {
        return false;
    }
    const struct recon_html_field *d = recon_html_field_at(page, index);
    if (d != NULL && d->kind == RECON_HTML_FIELD_FILE) {
        const struct recon_form_value *live = &values[index];
        /*
         * An empty part with an empty filename when nothing was chosen, which
         * is what a browser sends and is not a stand-in for anything. The
         * server's question is "did this form have a picture field", and the
         * answer is yes, and it was left empty.
         */
        *out_filename = live->file_name != NULL ? live->file_name : "";
        *out_text = live->file_bytes != NULL ? live->file_bytes : "";
        *out_length = live->file_bytes != NULL ? live->file_length : 0;
        return true;
    }
    *out_filename = NULL;
    *out_text = sends;
    *out_length = strlen(sends);
    return true;
}

/*
 * A boundary that appears nowhere in what it is about to separate.
 *
 * The same problem `recon_smtp_message.c` solves for a letter, solved the same
 * way and for the same reason: a boundary that also occurs inside the content
 * splits the request at that point instead, and somebody who can choose the
 * bytes of an upload can end the body early and append parts of their own --
 * fields the person filling the form never saw.
 *
 * The usual answer is a long random string and the argument that a collision
 * is unlikely. That argument is fine against accident and worthless against
 * somebody who has read this file. So the string is CHECKED: built, looked for
 * in everything it will separate, and rebuilt with a different number if it is
 * found. Certain rather than probable, for one pass over the content.
 *
 * Unlike the letter's, this one has to search the file's bytes in earnest.
 * A letter base64-encodes its attachments, so the search there outlives a
 * change of encoding rather than catching anything today; a form sends the
 * file raw, and a file of saved HTTP requests is a perfectly ordinary thing
 * for somebody to upload.
 */
static bool choose_a_boundary(const struct recon_html_document *page, int form,
        int submitter, const struct recon_form_value *values, int value_count,
        char *out, size_t out_size) {
    int fields = recon_html_field_count(page);

    for (int attempt = 0; attempt < 1000; attempt++) {
        int written = snprintf(out, out_size, "----ReconOSForm%d", attempt);
        if (written < 0 || (size_t)written >= out_size) {
            return false;
        }

        bool clashes = false;
        for (int i = 0; i < fields && i < value_count && !clashes; i++) {
            const struct recon_html_field *d = recon_html_field_at(page, i);
            if (d == NULL || d->form != form) {
                continue;
            }
            const char *text = NULL, *filename = NULL;
            size_t length = 0;
            if (!part_of(page, i, values, value_count, submitter, &text,
                    &length, &filename)) {
                continue;
            }
            if (strstr(d->name, out) != NULL ||
                    bytes_contain(text, length, out) ||
                    (filename != NULL && strstr(filename, out) != NULL)) {
                clashes = true;
            }
        }
        if (!clashes) {
            return true;
        }
    }

    /*
     * A thousand different boundaries all present in one submission is not
     * something that happens by accident, so this is somebody trying. Refused
     * rather than sent with the thousand-and-first.
     */
    return false;
}

char *recon_form_body_multipart(const struct recon_html_document *page,
        int form, int submitter, const struct recon_form_value *values,
        int value_count, size_t limit, char *boundary_out,
        size_t boundary_size, size_t *out_length, int *out_count,
        bool *out_secret) {
    if (out_length != NULL) {
        *out_length = 0;
    }
    if (out_count != NULL) {
        *out_count = 0;
    }
    if (out_secret != NULL) {
        *out_secret = false;
    }
    if (page == NULL || values == NULL || boundary_out == NULL ||
            boundary_size < RECON_FORM_BOUNDARY_MAX || limit < 2) {
        return NULL;
    }
    if (!choose_a_boundary(page, form, submitter, values, value_count,
            boundary_out, boundary_size)) {
        return NULL;
    }

    char *body = calloc(1, limit);
    if (body == NULL) {
        return NULL;
    }
    size_t used = 0;
    int count = 0;
    bool secret = false;

    int fields = recon_html_field_count(page);
    for (int i = 0; i < fields && i < value_count; i++) {
        const struct recon_html_field *d = recon_html_field_at(page, i);
        if (d == NULL || d->form != form) {
            continue;
        }
        const char *text = NULL, *filename = NULL;
        size_t length = 0;
        if (!part_of(page, i, values, value_count, submitter, &text, &length,
                &filename)) {
            continue;
        }

        bool ok = put(body, limit, &used, "--")
            && put(body, limit, &used, boundary_out)
            && put(body, limit, &used, "\r\n")
            && put(body, limit, &used, "Content-Disposition: form-data; name=\"")
            && put_quoted(body, limit, &used, d->name)
            && put(body, limit, &used, "\"");
        if (ok && filename != NULL) {
            ok = put(body, limit, &used, "; filename=\"")
                && put_quoted(body, limit, &used, filename)
                && put(body, limit, &used, "\"\r\nContent-Type: ")
                && put(body, limit, &used, recon_media_type(filename));
        }
        ok = ok
            && put(body, limit, &used, "\r\n\r\n")
            && put_bytes(body, limit, &used, text, length)
            && put(body, limit, &used, "\r\n");
        if (!ok) {
            free(body);
            return NULL;
        }

        count++;
        if (d->secret) {
            secret = true;
        }
    }

    /*
     * The closing boundary goes on even when nothing was sent. A body of no
     * parts is still a well-formed multipart body, and the alternative -- an
     * empty body with a multipart content type -- is the shape a server reads
     * as a broken request rather than an empty form.
     */
    if (!put(body, limit, &used, "--") ||
            !put(body, limit, &used, boundary_out) ||
            !put(body, limit, &used, "--\r\n")) {
        free(body);
        return NULL;
    }

    if (out_length != NULL) {
        *out_length = used;
    }
    if (out_count != NULL) {
        *out_count = count;
    }
    if (out_secret != NULL) {
        *out_secret = secret;
    }
    return body;
}

bool recon_form_get_address(struct recon_http_url *where, const char *body) {
    if (where == NULL || body == NULL) {
        return false;
    }

    char path[RECON_HTTP_URL_MAX];
    snprintf(path, sizeof(path), "%s", where->path);

    char *query = strchr(path, '?');
    if (query != NULL) {
        *query = '\0';
    }
    if (path[0] == '\0') {
        /* An address with no path at all asks for the root, which is what a
         * bare "example.com" means everywhere. */
        snprintf(path, sizeof(path), "/");
    }

    char full[RECON_HTTP_URL_MAX];
    int wrote = snprintf(full, sizeof(full), "%s?%s", path, body);
    if (wrote < 0 || (size_t)wrote >= sizeof(full)) {
        return false;
    }

    snprintf(where->path, sizeof(where->path), "%s", full);

    /*
     * A fragment is not part of what is asked of the server, and a submission
     * is a new request rather than a place on the page that made it. Carrying
     * one over would scroll the answer to a heading somebody was reading on
     * the form.
     */
    where->fragment[0] = '\0';
    return true;
}
