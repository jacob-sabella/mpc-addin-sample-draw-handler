/* mpc_sample_draw.h: the interface of mpc-addin-sample-draw-handler, for anything inside MPC that draws on the MPC Sample's
 * screen: plugins and other addins alike.
 *
 * The addin is preloaded into MPC and is the only thing that writes to the display. A plugin or another addin opens a client, draws into the client's own off-screen layer, and asks for the screen. The last client to ask
 * gets the whole screen and keeps it until it releases it (when its page closes) or another client asks; then MPC's own
 * screen comes back, exactly as MPC last drew it. Nothing is handed over automatically on release.
 *
 * No link dependency: look the entry point up at run time, so a plugin still loads where the addin isn't installed.
 *
 *     const mpc_sdraw_api *(*get)(int) = (const mpc_sdraw_api *(*)(int))dlsym(RTLD_DEFAULT, MPC_SDRAW_ENTRY);
 *     const mpc_sdraw_api *api = get ? get(MPC_SDRAW_API_VERSION) : NULL;   // NULL: no addin, or too old
 *
 * Another addin: preloaded libraries start in an order the draw handler doesn't control, so get() can return NULL while the
 * draw handler hasn't started yet. Look it up from your own thread, and again a little later if it was NULL.
 *
 * The page button (a long hold of PAD FX by default) cycles the screen through the open clients in the order they opened:
 * MPC -> first -> next -> ... -> MPC. A client given the screen that way gets the focus callback.
 *
 * Threads: every call may come from any thread except MPC's audio thread, and is short (a present copies at most one
 * screen of pixels). The focus callback reports only changes the client did not make itself: another client's request
 * took the screen, or the control interface granted or took it. It runs on the thread that made the change, never with
 * the addin's lock held, and may call back into the API. */
#ifndef MPC_SAMPLE_DRAW_H
#define MPC_SAMPLE_DRAW_H
#include <stdint.h>

#define MPC_SDRAW_ENTRY "mpc_sample_draw_api"
#define MPC_SDRAW_API_VERSION 3   /* 2 added set_input, 3 show and the dial press; asking for an older version gets the same struct */

typedef struct mpc_sdraw_client mpc_sdraw_client;

/* has_screen: 1 when this client just got the screen, 0 when it just lost it. */
typedef void (*mpc_sdraw_focus_fn)(void *user, int has_screen);

/* Input for the client that has the screen (API version 2). The controls named in the addin's settings go to the client
 * instead of MPC while it has the screen; everything else (pads, transport, the page button, other buttons) still goes to
 * MPC. On the MPC Sample: the three keys above the screen, the data dial and the knobs K1-K3. */
enum {
    MPC_SDRAW_EV_KEY = 1,    /* id: soft key 0, 1, 2 (left to right), or 3: the data dial pressed in ("enter", API 3);
                              * value: 1 pressed, 0 let go */
    MPC_SDRAW_EV_DIAL = 2,   /* value: steps turned, + clockwise */
    MPC_SDRAW_EV_KNOB = 3,   /* id: knob 0, 1, 2 (K1-K3); value: its position, 0-127, after a turn */
    MPC_SDRAW_EV_KNOB_AT = 4,   /* where knob id already is (value 0-127), sent when the client gets the screen: nothing
                                 * turned, so a client that catches values (pickup) only learns the position */
};
typedef struct { int type, id, value; } mpc_sdraw_event;
/* Runs on the addin's own thread, never with its lock held: may call the API (not close). Keep it short. */
typedef void (*mpc_sdraw_input_fn)(void *user, const mpc_sdraw_event *e);

typedef struct {
    int version;              /* MPC_SDRAW_API_VERSION of the addin */
    int width, height;        /* the screen and every layer: 320 x 240 on the MPC Sample */

    /* A client with a cleared (black) layer. name is shown by the control interface (copied). NULL when the addin
     * has no screen (not the MPC Sample, or disabled). */
    mpc_sdraw_client *(*open)(const char *name, mpc_sdraw_focus_fn focus, void *user);
    /* Releases the screen if held. No callback for this client after it returns. Call before the plugin unloads. */
    void (*close)(mpc_sdraw_client *c);

    /* Ask for the screen: the client gets it at once (the previous owner is told it lost it). Returns 0, or -1 when
     * the display isn't known yet (MPC hasn't drawn since the addin started): ask again later. */
    int (*request)(mpc_sdraw_client *c);
    /* Give the screen back to MPC (no-op unless this client holds it). */
    void (*release)(mpc_sdraw_client *c);
    int (*has_screen)(mpc_sdraw_client *c);

    /* The client's layer: width x height pixels, 0x00RRGGBB, row-major, *stride pixels per row. Always valid while
     * the client is open, whether or not it has the screen. */
    uint32_t *(*pixels)(mpc_sdraw_client *c, int *stride);
    /* Show what changed in the layer (a rectangle, clipped to the screen; w or h <= 0 means the whole screen). Copies
     * it to the screen and refreshes the panel when this client has the screen; otherwise does nothing (the whole
     * layer is shown when the client gets the screen). */
    void (*present)(mpc_sdraw_client *c, int x, int y, int w, int h);

    /* Version 2 (check version >= 2 first): take the controls while this client has the screen; fn NULL gives them
     * back to MPC. close() waits for a callback in progress, and none comes after it returns. */
    void (*set_input)(mpc_sdraw_client *c, mpc_sdraw_input_fn fn, void *user);

    /* Version 3: give the screen to the open client named name (the newest, if several have that name; not c itself),
     * e.g. a rack opening a plugin's own page. That client gets the focus callback; c is told it lost the screen. While
     * it has the screen, the page button goes back to c instead of cycling on. 0, or -1 (no such client, or no display
     * yet). */
    int (*show)(mpc_sdraw_client *c, const char *name);
} mpc_sdraw_api;

#endif
