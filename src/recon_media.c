/*
 * What kind of file something is, by its name. See include/recon_media.h.
 *
 * It began as a static helper inside the mail composer. It moved out when the
 * form encoder needed the same answers, and it landed here rather than in
 * `recon_http.c` because a program wanting a media type should not link an
 * HTTP client to get one.
 */

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "recon_media.h"

/* What kind of file this is, by its name. See include/recon_media.h. */
const char *recon_media_type(const char *name) {
    static const struct { const char *ext; const char *type; } TYPES[] = {
        { ".txt",  "text/plain"       }, { ".md",   "text/plain"       },
        { ".csv",  "text/csv"         }, { ".html", "text/html"        },
        { ".png",  "image/png"        }, { ".jpg",  "image/jpeg"       },
        { ".jpeg", "image/jpeg"       }, { ".gif",  "image/gif"        },
        { ".bmp",  "image/bmp"        }, { ".webp", "image/webp"       },
        { ".pdf",  "application/pdf"  }, { ".zip",  "application/zip"  },
        { ".wav",  "audio/wav"        }, { ".mp3",  "audio/mpeg"       },
        { ".mp4",  "video/mp4"        }, { ".json", "application/json" },
    };

    if (name == NULL) {
        return "application/octet-stream";
    }
    const char *dot = strrchr(name, '.');
    if (dot != NULL) {
        for (size_t i = 0; i < sizeof(TYPES) / sizeof(TYPES[0]); i++) {
            size_t n = strlen(TYPES[i].ext);
            if (strlen(dot) == n) {
                bool same = true;
                for (size_t j = 0; j < n && same; j++) {
                    char a = dot[j], b = TYPES[i].ext[j];
                    if (a >= 'A' && a <= 'Z') {
                        a = (char)(a - 'A' + 'a');
                    }
                    same = (a == b);
                }
                if (same) {
                    return TYPES[i].type;
                }
            }
        }
    }
    return "application/octet-stream";
}
