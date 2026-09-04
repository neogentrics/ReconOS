/*
 * The text clipboard. See include/recon_clip.h.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>

#include "recon_clip.h"

/*
 * A cap, because a clipboard holds whatever somebody selected and somebody
 * can select a whole file. Large enough that no ordinary copy hits it, small
 * enough that a mistake does not take the system's memory with it.
 */
#define CLIP_MAX (1024 * 1024)

static char *g_text;
static size_t g_length;

bool recon_clip_set_text(const char *text, size_t length) {
    free(g_text);
    g_text = NULL;
    g_length = 0;

    if (text == NULL || length == 0) {
        return true;    /* Emptying it is a success, not a failure to fill it. */
    }
    if (length > CLIP_MAX) {
        length = CLIP_MAX;
    }

    /* One byte over, always terminated, so callers that treat it as a string
     * are right without having to be careful. */
    g_text = malloc(length + 1);
    if (g_text == NULL) {
        return false;
    }

    memcpy(g_text, text, length);
    g_text[length] = '\0';
    g_length = length;
    return true;
}

const char *recon_clip_text(void) {
    return g_text != NULL ? g_text : "";
}

size_t recon_clip_length(void) {
    return g_length;
}

bool recon_clip_empty(void) {
    return g_length == 0;
}

void recon_clip_finish(void) {
    free(g_text);
    g_text = NULL;
    g_length = 0;
}
