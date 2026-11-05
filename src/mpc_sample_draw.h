/* mpc_sample_draw.h: the interface of mpc-addin-sample-draw-handler, for plugins that draw on the MPC Sample's screen.
 *
 * The addin is preloaded into MPC and is the only thing that writes to the display. A plugin (or anything else loaded
 * in MPC) opens a client, draws into the client's own off-screen layer, and asks for the screen. The last client to ask
 * gets the whole screen and keeps it until it releases it (when its page closes) or another client asks; then MPC's own
 * screen comes back, exactly as MPC last drew it. Nothing is handed over automatically on release.
 *
 * No link dependency: look the entry point up at run time, so a plugin still loads where the addin isn't installed.
 *
 *     const mpc_sdraw_api *(*get)(int) = (const mpc_sdraw_api *(*)(int))dlsym(RTLD_DEFAULT, MPC_SDRAW_ENTRY);
 *     const mpc_sdraw_api *api = get ? get(MPC_SDRAW_API_VERSION) : NULL;   // NULL: no addin, or too old
 *
 * Threads: every call may come from any thread except MPC's audio thread, and is short (a present copies at most one
 * screen of pixels). The focus callback reports only changes the client did not make itself: another client's request
 * took the screen, or the control interface granted or took it. It runs on the thread that made the change, never with
 * the addin's lock held, and may call back into the API. */
#ifndef MPC_SAMPLE_DRAW_H
#define MPC_SAMPLE_DRAW_H
#include <stdint.h>

#define MPC_SDRAW_ENTRY "mpc_sample_draw_api"
#define MPC_SDRAW_API_VERSION 1

typedef struct mpc_sdraw_client mpc_sdraw_client;

/* has_screen: 1 when this client just got the screen, 0 when it just lost it. */
typedef void (*mpc_sdraw_focus_fn)(void *user, int has_screen);

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
} mpc_sdraw_api;

#endif
