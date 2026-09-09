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
