/* scrobble.h — Last.fm + ListenBrainz scrobbling.
 * Last.fm uses full API-key auth (device flow: token -> browser -> session).
 * ListenBrainz just needs a personal API token. */
#ifndef MIKA_SCROBBLE_H
#define MIKA_SCROBBLE_H

#include "queue.h"

void scrobble_init(void);
void scrobble_shutdown(void);

/* fire-and-forget (background threads, never block the UI) */
void scrobble_now_playing(const Meta *m);
void scrobble_submit(const Meta *m, int played_secs);

/* last.fm auth: opens browser, polls for session. returns immediately */
void scrobble_auth_start(void);
int  scrobble_auth_state(void);    /* 0 idle, 1 waiting for browser, 2 done, -1 failed */
const char *scrobble_auth_msg(void);

#endif
