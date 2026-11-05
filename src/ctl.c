/* mpc-addin-sample-draw-handler, the control side: settings, startup inside MPC, a small HTTP interface on the device
 * (127.0.0.1 by default) and a built-in test-pattern client.
 *   GET /status         the clients, the owner and the display (JSON)
 *   GET /grant?id=N     give client N the screen
 *   GET /release        give MPC its screen back
 *   GET /test?on=1|0    open the test-pattern client and give it the screen, or close it
 *   GET /cycle          what the page button does: MPC -> first client -> next -> ... -> MPC
 *   GET /input?key=K | ?dial=N | ?knob=K&value=V
 *                       an input event for the client with the screen, as if from the soft key K (0-2, a press and
 *                       release), N dial steps or knob K at V (0-127): to drive a page without touching the device */
#define _GNU_SOURCE
#include "sdraw_int.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <dlfcn.h>

static struct { int enabled, port, page_note, page_hold_ms, log_buttons; char bind[80]; int keys[4], knobs[3], dial_cc; } C = {
    1, 6740, 1, 800, 1, "127.0.0.1", { 12, 13, 14, 111 }, { 16, 17, 18 }, 100 };

/* "a,b,c": three numbers 0-127, or -1 for none */
static void three(const char *v, int out[3])
{
    int t[3] = { -1, -1, -1 }, n = 0;
    for (const char *p = v; *p && n < 3; ) {
        char *e; long x = strtol(p, &e, 10);
        if (e == p) break;
        if (x < -1 || x > 127) return;
        t[n++] = (int)x;
        p = *e == ',' ? e + 1 : e;
        if (!*e) break;
    }
    if (n) for (int i = 0; i < 3; i++) out[i] = t[i];
}

static void conf_load(const char *path)
{
    FILE *f = fopen(path, "re");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (isspace((unsigned char)*p)) p++;
        if (!*p || *p == '#') continue;
        char k[32], v[64];
        if (sscanf(p, "%31[a-z_] = %63s", k, v) != 2) { LOG("%s: not understood: %s", path, p); continue; }
        if (!strcmp(k, "enabled")) C.enabled = atoi(v) != 0;
        else if (!strcmp(k, "port")) { int n = atoi(v); if (n > 0 && n < 65536) C.port = n; }
        else if (!strcmp(k, "bind")) snprintf(C.bind, sizeof C.bind, "%s", v);
        else if (!strcmp(k, "page_note")) { int n = atoi(v); if (n >= -1 && n < 128) C.page_note = n; }
        else if (!strcmp(k, "page_home")) sdraw_set_home(strcmp(v, "-") ? v : "");
        else if (!strcmp(k, "page_hold_ms")) { int n = atoi(v); if (n >= 200 && n <= 5000) C.page_hold_ms = n; }
        else if (!strcmp(k, "soft_keys")) three(v, C.keys);
        else if (!strcmp(k, "knobs")) three(v, C.knobs);
        else if (!strcmp(k, "dial_press")) { int n = atoi(v); if (n >= -1 && n < 128) C.keys[3] = n; }
        else if (!strcmp(k, "dial_cc")) { int n = atoi(v); if (n >= -1 && n < 128) C.dial_cc = n; }
        else if (!strcmp(k, "log_buttons")) { int l = atoi(v); C.log_buttons = l < 0 ? 0 : l > 2 ? 2 : l; }
        else LOG("%s: unknown key %s\n", path, k);
    }
    fclose(f);
}

static void addin_dir(char *out, size_t n)
{
    Dl_info di;
    out[0] = 0;
    if (dladdr((void *)addin_dir, &di) && di.dli_fname) {
        snprintf(out, n, "%s", di.dli_fname);
        char *s = strrchr(out, '/');
        if (s) *s = 0; else out[0] = 0;
    }
}

/* ---- the test pattern: colour bars, a frame and a box that moves, ~15 frames a second while it has the screen ---- */
static const mpc_sdraw_api *A;
static mpc_sdraw_client *test_c;
static atomic_int test_run;
static pthread_t test_t;
static pthread_mutex_t test_mu = PTHREAD_MUTEX_INITIALIZER;

static void fill(uint32_t *px, int st, int x0, int y0, int w, int h, uint32_t col)
{
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if (x >= 0 && y >= 0 && x < A->width && y < A->height) px[y * st + x] = col;
}

static void *test_thread(void *arg)
{
    (void)arg;
    static const uint32_t bars[8] = { 0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x101010 };
    int st, f = 0, last = -1;
    uint32_t *px = A->pixels(test_c, &st);
    int bw = A->width / 8;
    for (int i = 0; i < 8; i++) fill(px, st, i * bw, 0, bw, 140, bars[i]);
    fill(px, st, 0, 140, A->width, A->height - 140, 0x0B0E14);
    fill(px, st, 0, 0, A->width, 3, 0xC42E8C); fill(px, st, 0, A->height - 3, A->width, 3, 0xC42E8C);
    fill(px, st, 0, 0, 3, A->height, 0x2EC4B6); fill(px, st, A->width - 3, 0, 3, A->height, 0x2EC4B6);
    A->present(test_c, 0, 0, 0, 0);
    while (atomic_load(&test_run)) {
        int x = 10 + (f * 4) % (A->width - 60);
        if (last >= 0) fill(px, st, last, 170, 40, 40, 0x0B0E14);
        fill(px, st, x, 170, 40, 40, 0xFF8000);
        int x0 = last < 0 ? x : (last < x ? last : x);
        A->present(test_c, x0, 170, 44 + (last < 0 ? 0 : abs(x - last)), 40);
        last = x; f++;
        struct timespec ts = { 0, 66 * 1000000L };
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static int test_set(int on)
{
    pthread_mutex_lock(&test_mu);
    int r = 0;
    if (on && !test_c) {
        test_c = A->open("test pattern", NULL, NULL);
        if (!test_c) r = -1;
        else if (A->request(test_c)) { A->close(test_c); test_c = NULL; r = -2; }
        else { atomic_store(&test_run, 1); pthread_create(&test_t, NULL, test_thread, NULL); }
    } else if (!on && test_c) {
        atomic_store(&test_run, 0);
        pthread_join(test_t, NULL);
        A->close(test_c);
        test_c = NULL;
    }
    pthread_mutex_unlock(&test_mu);
    return r;
}

/* ---- HTTP ---- */
static void reply(int fd, const char *status, const char *type, const char *body)
{
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     status, type, strlen(body));
    if (write(fd, h, (size_t)n) < 0 || write(fd, body, strlen(body)) < 0) return;
}

static int query_int(const char *q, const char *key, int def)
{
    if (!q) return def;
    size_t kl = strlen(key);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL)
        if (!strncmp(p, key, kl) && p[kl] == '=') return atoi(p + kl + 1);
    return def;
}

static void serve(int fd)
{
    char req[1024];
    ssize_t n = read(fd, req, sizeof req - 1);
    if (n <= 0) return;
    req[n] = 0;
    char path[256];
    if (sscanf(req, "GET %255s", path) != 1) { reply(fd, "405 Method Not Allowed", "text/plain", "GET only\n"); return; }
    char *q = strchr(path, '?');
    if (q) *q++ = 0;
    static char body[8192];
    if (!strcmp(path, "/status")) {
        sdraw_status_json(body, sizeof body);
        reply(fd, "200 OK", "application/json", body);
    } else if (!strcmp(path, "/grant")) {
        int r = sdraw_grant(query_int(q, "id", -1));
        reply(fd, r ? "409 Conflict" : "200 OK", "text/plain",
              r == 0 ? "ok\n" : r == -1 ? "the display isn't known yet: MPC hasn't refreshed the screen since the addin started\n" : "no such client\n");
    } else if (!strcmp(path, "/release")) {
        sdraw_take_back();
        reply(fd, "200 OK", "text/plain", "ok\n");
    } else if (!strcmp(path, "/cycle")) {
        sdraw_cycle();
        reply(fd, "200 OK", "text/plain", "ok\n");
    } else if (!strcmp(path, "/test")) {
        int r = test_set(query_int(q, "on", 1) != 0);
        reply(fd, r ? "409 Conflict" : "200 OK", "text/plain",
              r == 0 ? "ok\n" : r == -2 ? "the display isn't known yet: MPC hasn't refreshed the screen since the addin started\n" : "no screen\n");
    } else if (!strcmp(path, "/input")) {
        int key = query_int(q, "key", -1), dial = query_int(q, "dial", 0), knob = query_int(q, "knob", -1);
        if (key >= 0 && key < 4) {
            mpc_sdraw_event e = { MPC_SDRAW_EV_KEY, key, 1 }; sdraw_deliver(&e);
            e.value = 0; sdraw_deliver(&e);
        } else if (dial) {
            mpc_sdraw_event e = { MPC_SDRAW_EV_DIAL, 0, dial }; sdraw_deliver(&e);
        } else if (knob >= 0 && knob < 3) {
            int v = query_int(q, "value", -1);
            if (v < 0 || v > 127) { reply(fd, "400 Bad Request", "text/plain", "knob needs value=0-127\n"); return; }
            mpc_sdraw_event e = { MPC_SDRAW_EV_KNOB, knob, v }; sdraw_deliver(&e);
        } else { reply(fd, "400 Bad Request", "text/plain", "key=0-2, dial=N or knob=0-2&value=0-127\n"); return; }
        reply(fd, "200 OK", "text/plain", sdraw_input_wanted() ? "ok\n" : "ok (the client with the screen takes no input)\n");
    } else {
        reply(fd, "404 Not Found", "text/plain", "/status /grant?id=N /release /cycle /test?on=1|0 /input?key=K|dial=N|knob=K&value=V\n");
    }
}

static int listen_fd = -1;

static void *server_thread(void *arg)
{
    (void)arg;
    for (;;) {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; LOG("accept: %s\n", strerror(errno)); return NULL; }
        struct timeval tv = { 2, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        serve(fd);
        close(fd);
    }
}

static int start_server(void)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)C.port) };
    if (inet_pton(AF_INET, C.bind, &a.sin_addr) != 1) { LOG("bind=%s is not an IPv4 address, using 127.0.0.1\n", C.bind); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); }
    listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (listen_fd < 0 || bind(listen_fd, (struct sockaddr *)&a, sizeof a) || listen(listen_fd, 4)) {
        LOG("can't listen on %s:%d: %s\n", C.bind, C.port, strerror(errno));
        return -1;
    }
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    pthread_t t;
    int r = pthread_create(&t, NULL, server_thread, NULL);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (r) { LOG("can't start the control thread: %s\n", strerror(r)); return -1; }
    pthread_detach(t);
    return 0;
}

/* ---- startup ---- */
static int is_sample(void)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return 0;
    exe[n] = 0;
    const char *b = strrchr(exe, '/');
    return !strcmp(b ? b + 1 : exe, "MPC Sample");
}

__attribute__((visibility("default"))) int mpc_sample_draw_handler_start(void)
{
    static atomic_int started;
    if (atomic_exchange(&started, 1)) return 0;
    char path[PATH_MAX];
    const char *env = getenv("MPC_SDRAW_CONF");
    if (env) snprintf(path, sizeof path, "%s", env);
    else { char dir[PATH_MAX - 40]; addin_dir(dir, sizeof dir); snprintf(path, sizeof path, "%s/mpc_sample_draw_handler.conf", dir[0] ? dir : "."); }
    conf_load(path);
    if (!C.enabled) { LOG("disabled in %s\n", path); return -1; }
    sdraw_enable();
    A = mpc_sample_draw_api(MPC_SDRAW_API_VERSION);
    input_controls(C.keys, C.knobs, C.dial_cc);
    if (C.page_note >= 0) { input_config(C.page_note, C.page_hold_ms, C.log_buttons); input_enable(); }
    start_server();
    LOG("%s: control on http://%s:%d/ (status, grant, release, test); page button: note %d held %d ms\n", ADDIN_VERSION, C.bind,
        C.port, C.page_note, C.page_hold_ms);
    return 0;
}

__attribute__((constructor)) static void addin_init(void)
{
    if (getenv("MPC_SDRAW_DISABLE") || !is_sample()) return;
    mpc_sample_draw_handler_start();
}
