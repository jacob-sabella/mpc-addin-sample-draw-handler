/* mpc-addin-sample-draw-handler, the core: owns the MPC Sample's display on behalf of the plugins that draw on it.
 *
 * MPC draws its UI into one dumb framebuffer and tells the SPI panel's driver which rectangles changed
 * (DRM_IOCTL_MODE_DIRTYFB); the driver sends only those to the glass (docs/NOTES.md in mpc-vst-plugins, 2026-11-04). The
 * addin wraps ioctl() (MPC reaches it through libdrm's drmIoctl), so it sees every refresh, learns MPC's display fd (the
 * DRM master: only it may refresh) and the framebuffer, and maps that framebuffer itself.
 *
 * While a client owns the screen, each refresh MPC asks for first saves MPC's new pixels of that rectangle into a shadow
 * copy, then paints the owner's layer over them, so the refresh sends the owner's pixels. The owner's own present() copies
 * its rectangle in and refreshes it on MPC's fd. Release puts the shadow back and refreshes the whole screen: MPC's screen
 * as it last drew it. Known gap: pixels MPC paints and an owner's present() overwrites before MPC's refresh never reach
 * the shadow; they show the owner's pixels after release until MPC redraws them. */
#define _GNU_SOURCE
#include "sdraw_int.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* ---- the DRM uapi structures used (drm.h, drm_mode.h: stable kernel ABI) ---- */
struct drm_mode_fb_cmd2 { uint32_t fb_id, width, height, pixel_format, flags, handles[4], pitches[4], offsets[4]; uint64_t modifier[4]; };
struct drm_mode_map_dumb { uint32_t handle, pad; uint64_t offset; };
struct drm_gem_close { uint32_t handle, pad; };
struct drm_clip_rect { unsigned short x1, y1, x2, y2; };
struct drm_mode_fb_dirty_cmd { uint32_t fb_id, flags, color, num_clips; uint64_t clips_ptr; };
struct drm_mode_crtc_page_flip { uint32_t crtc_id, fb_id, flags, reserved; uint64_t user_data; };
struct drm_mode_crtc_head { uint64_t set_connectors_ptr; uint32_t count_connectors, crtc_id, fb_id; };
#define DRM_TYPE 'd'
#define NR_PAGE_FLIP 0xB0
#define NR_DIRTYFB   0xB1
#define NR_SETCRTC   0xA2
#define DRM_IOCTL_GEM_CLOSE     _IOW('d', 0x09, struct drm_gem_close)
#define DRM_IOCTL_MODE_DIRTYFB  _IOWR('d', 0xB1, struct drm_mode_fb_dirty_cmd)
#define DRM_IOCTL_MODE_MAP_DUMB _IOWR('d', 0xB3, struct drm_mode_map_dumb)
#define DRM_IOCTL_MODE_GETFB2   _IOWR('d', 0xCE, struct drm_mode_fb_cmd2)
#define FOURCC_XR24 0x34325258u
#define FOURCC_AR24 0x34325241u

struct mpc_sdraw_client {
    int id, used;
    char name[48];
    mpc_sdraw_focus_fn focus;
    void *user;
    mpc_sdraw_input_fn input; void *input_user;
    int back;                      /* shown by client id back (show()): the page button returns there */
    uint32_t layer[SDRAW_W * SDRAW_H];
};

static struct {
    pthread_mutex_t mu;
    atomic_int on;                 /* sdraw_enable() was called */
    int fd;                        /* MPC's display fd (from its refreshes), -1 until seen */
    uint32_t fb;                   /* the framebuffer MPC refreshes */
    uint32_t *px; size_t len; int pitch;   /* that framebuffer, mapped (pitch in pixels) */
    uint32_t shadow[SDRAW_W * SDRAW_H];    /* MPC's screen while a client owns it */
    struct mpc_sdraw_client *cl[SDRAW_MAX_CLIENTS];
    struct mpc_sdraw_client *owner;
    int next_id;
    /* counters for the control interface */
    unsigned long mpc_dirty, own_dirty, flips, dirty_err;
    char err[96];
    /* the first 30 s of MPC's DRM ioctls by number, logged once: what MPC really calls */
    unsigned hist[256]; long long hist_t0; int hist_done;
    /* tests: a file stands in for the framebuffer, and refreshes are only counted */
    const char *test_fb;
} S = { .mu = PTHREAD_MUTEX_INITIALIZER, .fd = -1, .next_id = 1 };

static int (*real_ioctl)(int, unsigned long, ...);
static pthread_mutex_t deliver_mu = PTHREAD_MUTEX_INITIALIZER;   /* held while an input callback runs */

static void resolve(void)
{
    if (!real_ioctl) real_ioctl = (int (*)(int, unsigned long, ...))dlsym(RTLD_NEXT, "ioctl");
}

static long long now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

/* ---- the framebuffer ---- */
static void unmap_fb(void)
{
    if (S.px) munmap(S.px, S.len);
    S.px = NULL; S.len = 0; S.fb = 0;
}

/* Map MPC's framebuffer fb (lock held). 0 when mapped. */
static int map_fb(uint32_t fb)
{
    if (S.px && S.fb == fb) return 0;
    unmap_fb();
    if (S.test_fb) {
        int f = open(S.test_fb, O_RDWR | O_CLOEXEC);
        if (f < 0) { snprintf(S.err, sizeof S.err, "test framebuffer: %s", strerror(errno)); return -1; }
        S.len = (size_t)SDRAW_W * SDRAW_H * 4;
        void *p = mmap(NULL, S.len, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
        close(f);
        if (p == MAP_FAILED) { S.len = 0; snprintf(S.err, sizeof S.err, "test framebuffer: mmap failed"); return -1; }
        S.px = p; S.pitch = SDRAW_W; S.fb = fb;
        return 0;
    }
    struct drm_mode_fb_cmd2 f;
    memset(&f, 0, sizeof f);
    f.fb_id = fb;
    if (real_ioctl(S.fd, DRM_IOCTL_MODE_GETFB2, &f)) { snprintf(S.err, sizeof S.err, "GETFB2: %s", strerror(errno)); return -1; }
    int bad = f.width != SDRAW_W || f.height != SDRAW_H || (f.pixel_format != FOURCC_XR24 && f.pixel_format != FOURCC_AR24) ||
              !f.handles[0] || f.pitches[0] < SDRAW_W * 4;
    void *p = MAP_FAILED;
    size_t len = (size_t)f.pitches[0] * f.height + f.offsets[0];
    if (!bad) {
        struct drm_mode_map_dumb md = { .handle = f.handles[0] };
        if (real_ioctl(S.fd, DRM_IOCTL_MODE_MAP_DUMB, &md) == 0)
            p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, S.fd, (off_t)md.offset);
    }
    if (f.handles[0]) { struct drm_gem_close gc = { .handle = f.handles[0] }; real_ioctl(S.fd, DRM_IOCTL_GEM_CLOSE, &gc); }
    if (bad) { snprintf(S.err, sizeof S.err, "framebuffer %ux%u format %08x is not the Sample's", f.width, f.height, f.pixel_format); return -1; }
    if (p == MAP_FAILED) { snprintf(S.err, sizeof S.err, "can't map framebuffer %u", fb); return -1; }
    S.px = (uint32_t *)((char *)p + f.offsets[0]);
    S.len = len; S.pitch = (int)(f.pitches[0] / 4); S.fb = fb;
    S.err[0] = 0;
    return 0;
}

/* ---- rectangles ---- */
typedef struct { int x0, y0, x1, y1; } rect;   /* x1, y1 exclusive */

static rect clip(int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return (rect){ 0, 0, SDRAW_W, SDRAW_H };
    rect r = { x, y, x + w, y + h };
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > SDRAW_W) r.x1 = SDRAW_W;
    if (r.y1 > SDRAW_H) r.y1 = SDRAW_H;
    return r;
}

static int empty(rect r) { return r.x0 >= r.x1 || r.y0 >= r.y1; }

/* Copy rectangle r of src (stride ss) into dst (stride ds). */
static void copy(uint32_t *dst, int ds, const uint32_t *src, int ss, rect r)
{
    size_t n = (size_t)(r.x1 - r.x0) * 4;
    for (int y = r.y0; y < r.y1; y++) memcpy(dst + (size_t)y * ds + r.x0, src + (size_t)y * ss + r.x0, n);
}

/* Refresh rectangle r of the panel on MPC's fd (lock held). */
static void refresh(rect r)
{
    if (empty(r)) return;
    S.own_dirty++;
    if (S.test_fb) return;
    struct drm_clip_rect c = { (unsigned short)r.x0, (unsigned short)r.y0, (unsigned short)r.x1, (unsigned short)r.y1 };
    struct drm_mode_fb_dirty_cmd d = { .fb_id = S.fb, .num_clips = 1, .clips_ptr = (uintptr_t)&c };
    if (real_ioctl(S.fd, DRM_IOCTL_MODE_DIRTYFB, &d)) S.dirty_err++;
}

static const rect FULL = { 0, 0, SDRAW_W, SDRAW_H };

/* MPC just drew rectangle r: keep its pixels, show the owner's (lock held). */
static void cover(rect r)
{
    if (empty(r)) return;
    copy(S.shadow, SDRAW_W, S.px, S.pitch, r);
    copy(S.px, S.pitch, S.owner->layer, SDRAW_W, r);
}

/* ---- MPC's display calls ---- */
static void note_hist(unsigned nr)
{
    if (S.hist_done) return;
    long long t = now_ms();
    if (!S.hist_t0) S.hist_t0 = t;
    if (t - S.hist_t0 < 30000) { S.hist[nr & 255]++; return; }
    S.hist_done = 1;
    char line[512];
    int n = 0;
    for (int i = 0; i < 256 && n < (int)sizeof line - 16; i++)
        if (S.hist[i]) n += snprintf(line + n, sizeof line - n, " 0x%02X:%u", i, S.hist[i]);
    LOG("MPC's DRM ioctls in its first 30 s (number:count):%s\n", n ? line : " none");
}

/* Before MPC's DRM ioctl goes out (lock held). */
static void on_mpc_drm(int fd, unsigned nr, void *arg)
{
    note_hist(nr);
    if (nr == NR_DIRTYFB) {
        const struct drm_mode_fb_dirty_cmd *d = arg;
        S.mpc_dirty++;
        S.fd = fd;
        if (map_fb(d->fb_id) || !S.owner) return;
        const struct drm_clip_rect *c = (const struct drm_clip_rect *)(uintptr_t)d->clips_ptr;
        if (!d->num_clips || !c) { cover(FULL); return; }
        for (uint32_t i = 0; i < d->num_clips; i++) cover(clip(c[i].x1, c[i].y1, c[i].x2 - c[i].x1, c[i].y2 - c[i].y1));
    } else if (nr == NR_PAGE_FLIP || nr == NR_SETCRTC) {
        uint32_t fb = nr == NR_PAGE_FLIP ? ((const struct drm_mode_crtc_page_flip *)arg)->fb_id
                                         : ((const struct drm_mode_crtc_head *)arg)->fb_id;
        if (nr == NR_PAGE_FLIP) S.flips++;
        if (!fb) return;
        S.fd = fd;
        if (map_fb(fb) || !S.owner) return;
        cover(FULL);   /* a new frame goes out whole: it carries the owner's layer */
    }
}

__attribute__((visibility("default"))) int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    resolve();
    if (((req >> 8) & 0xff) == DRM_TYPE && atomic_load_explicit(&S.on, memory_order_relaxed)) {
        unsigned nr = req & 0xff;
        if (nr == NR_DIRTYFB || nr == NR_PAGE_FLIP || nr == NR_SETCRTC || !S.hist_done) {
            pthread_mutex_lock(&S.mu);
            on_mpc_drm(fd, nr, arg);
            pthread_mutex_unlock(&S.mu);
        }
    }
    return real_ioctl(fd, req, arg);
}

/* ---- ownership ---- */
typedef struct { mpc_sdraw_client *c; int has; } note;

static void tell(note *ns, int n)
{
    for (int i = 0; i < n; i++)
        if (ns[i].c && ns[i].c->focus) ns[i].c->focus(ns[i].c->user, ns[i].has);
}

/* Make c the owner (lock held). The notes get who to tell, outside the lock; self: c asked itself (no note for c). */
static unsigned handovers;   /* times the screen changed hands (under S.mu): a release and request between two looks */

static int give(mpc_sdraw_client *c, int self, note *ns, int *n)
{
    if (!S.px) return -1;
    mpc_sdraw_client *prev = S.owner;
    if (!prev) copy(S.shadow, SDRAW_W, S.px, S.pitch, FULL);
    S.owner = c;
    c->back = 0;
    handovers++;
    copy(S.px, S.pitch, c->layer, SDRAW_W, FULL);
    refresh(FULL);
    if (prev && prev != c) ns[(*n)++] = (note){ prev, 0 };
    if (!self && prev != c) ns[(*n)++] = (note){ c, 1 };
    return 0;
}

/* Give MPC its screen back (lock held). */
static void restore(void)
{
    S.owner = NULL;
    if (!S.px) return;
    copy(S.px, S.pitch, S.shadow, SDRAW_W, FULL);
    refresh(FULL);
}

/* ---- the API ---- */
static mpc_sdraw_client *api_open(const char *name, mpc_sdraw_focus_fn focus, void *user)
{
    if (!atomic_load(&S.on)) return NULL;
    mpc_sdraw_client *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    snprintf(c->name, sizeof c->name, "%s", name ? name : "?");
    c->focus = focus; c->user = user;
    pthread_mutex_lock(&S.mu);
    int slot = -1;
    for (int i = 0; i < SDRAW_MAX_CLIENTS; i++) if (!S.cl[i]) { slot = i; break; }
    if (slot >= 0) { c->id = S.next_id++; c->used = 1; S.cl[slot] = c; }
    pthread_mutex_unlock(&S.mu);
    if (slot < 0) { free(c); LOG("too many clients (%d), \"%s\" refused\n", SDRAW_MAX_CLIENTS, name ? name : "?"); return NULL; }
    return c;
}

static void api_close(mpc_sdraw_client *c)
{
    if (!c) return;
    pthread_mutex_lock(&S.mu);
    if (S.owner == c) restore();
    for (int i = 0; i < SDRAW_MAX_CLIENTS; i++) if (S.cl[i] == c) S.cl[i] = NULL;
    pthread_mutex_unlock(&S.mu);
    pthread_mutex_lock(&deliver_mu);   /* a callback to c in progress finishes first */
    pthread_mutex_unlock(&deliver_mu);
    free(c);
}

static int api_request(mpc_sdraw_client *c)
{
    if (!c) return -1;
    note ns[2]; int n = 0;
    pthread_mutex_lock(&S.mu);
    int r = give(c, 1, ns, &n);
    pthread_mutex_unlock(&S.mu);
    tell(ns, n);
    return r;
}

static void api_release(mpc_sdraw_client *c)
{
    pthread_mutex_lock(&S.mu);
    if (c && S.owner == c) restore();
    pthread_mutex_unlock(&S.mu);
}

static int api_has_screen(mpc_sdraw_client *c)
{
    pthread_mutex_lock(&S.mu);
    int r = c && S.owner == c;
    pthread_mutex_unlock(&S.mu);
    return r;
}

static uint32_t *api_pixels(mpc_sdraw_client *c, int *stride)
{
    if (stride) *stride = SDRAW_W;
    return c ? c->layer : NULL;
}

static void api_present(mpc_sdraw_client *c, int x, int y, int w, int h)
{
    if (!c) return;
    pthread_mutex_lock(&S.mu);
    if (S.owner == c && S.px) {
        rect r = clip(x, y, w, h);
        if (!empty(r)) { copy(S.px, S.pitch, c->layer, SDRAW_W, r); refresh(r); }
    }
    pthread_mutex_unlock(&S.mu);
}

static void api_set_input(mpc_sdraw_client *c, mpc_sdraw_input_fn fn, void *user)
{
    if (!c) return;
    pthread_mutex_lock(&S.mu);
    c->input = fn; c->input_user = user;
    pthread_mutex_unlock(&S.mu);
}

int sdraw_input_wanted(void)
{
    pthread_mutex_lock(&S.mu);
    int r = S.owner && S.owner->input;
    pthread_mutex_unlock(&S.mu);
    return r;
}

/* Lock order deliver_mu -> S.mu, as in a callback that calls the API. close() removes the client under S.mu and then
 * waits on deliver_mu, so a client found here stays alive until its callback returns. */
int sdraw_input_owner(unsigned *gen)
{
    pthread_mutex_lock(&S.mu);
    int r = S.owner && S.owner->input ? S.owner->id : 0;
    if (gen) *gen = handovers;
    pthread_mutex_unlock(&S.mu);
    return r;
}

void sdraw_deliver(const mpc_sdraw_event *e)
{
    pthread_mutex_lock(&deliver_mu);
    pthread_mutex_lock(&S.mu);
    mpc_sdraw_input_fn fn = S.owner ? S.owner->input : NULL;
    void *user = S.owner ? S.owner->input_user : NULL;
    pthread_mutex_unlock(&S.mu);
    if (fn) fn(user, e);
    pthread_mutex_unlock(&deliver_mu);
}

static int api_show(mpc_sdraw_client *c, const char *name)
{
    if (!c || !name) return -1;
    note ns[2]; int n = 0, r = -1;
    pthread_mutex_lock(&S.mu);
    mpc_sdraw_client *t = NULL;
    for (int i = 0; i < SDRAW_MAX_CLIENTS; i++)
        if (S.cl[i] && S.cl[i] != c && !strcmp(S.cl[i]->name, name) && (!t || S.cl[i]->id > t->id)) t = S.cl[i];
    if (t && give(t, 0, ns, &n) == 0) { t->back = c->id; r = 0; }
    pthread_mutex_unlock(&S.mu);
    tell(ns, n);
    return r;
}

static const mpc_sdraw_api API = {
    .version = MPC_SDRAW_API_VERSION, .width = SDRAW_W, .height = SDRAW_H,
    .open = api_open, .close = api_close, .request = api_request, .release = api_release,
    .has_screen = api_has_screen, .pixels = api_pixels, .present = api_present, .set_input = api_set_input,
    .show = api_show,
};

__attribute__((visibility("default"))) const mpc_sdraw_api *mpc_sample_draw_api(int version)
{
    return version >= 1 && version <= MPC_SDRAW_API_VERSION && atomic_load(&S.on) ? &API : NULL;
}

void sdraw_enable(void)
{
    resolve();
    S.test_fb = getenv("MPC_SDRAW_TEST_FB");
    if (S.test_fb) {   /* tests: no display calls to wait for */
        pthread_mutex_lock(&S.mu);
        map_fb(1);
        pthread_mutex_unlock(&S.mu);
    }
    atomic_store(&S.on, 1);
}

/* ---- control ---- */
int sdraw_grant(int id)
{
    note ns[2]; int n = 0, r = -2;
    pthread_mutex_lock(&S.mu);
    for (int i = 0; i < SDRAW_MAX_CLIENTS; i++)
        if (S.cl[i] && S.cl[i]->id == id) { r = give(S.cl[i], 0, ns, &n); break; }
    pthread_mutex_unlock(&S.mu);
    tell(ns, n);
    return r;   /* 0, -1 no display yet, -2 no such client */
}

static char home_name[64] = "RACK";
void sdraw_set_home(const char *name) { snprintf(home_name, sizeof home_name, "%s", name ? name : ""); }

/* The page button. With the home page open (page_home, default RACK, the rack addin): MPC -> home -> MPC, and a page the
 * home page showed (show()) goes back to it. Without one: MPC -> each client in open order -> MPC. */
void sdraw_cycle(void)
{
    note ns[2]; int n = 0;
    pthread_mutex_lock(&S.mu);
    mpc_sdraw_client *next = NULL, *home = NULL;
    int after = S.owner ? S.owner->id : 0;
    for (int i = 0; i < SDRAW_MAX_CLIENTS && home_name[0]; i++)
        if (S.cl[i] && !strcmp(S.cl[i]->name, home_name) && (!home || S.cl[i]->id < home->id)) home = S.cl[i];
    if (S.owner && S.owner->back)   /* shown by another client: back to it */
        for (int i = 0; i < SDRAW_MAX_CLIENTS; i++) if (S.cl[i] && S.cl[i]->id == S.owner->back) next = S.cl[i];
    if (home) { if (!next && !S.owner) next = home; }   /* home open: it toggles; any other page closes */
    else
        for (int i = 0; i < SDRAW_MAX_CLIENTS && !next; i++)   /* the client with the lowest id above the owner's: open order */
            if (S.cl[i] && S.cl[i]->id > after && (!next || S.cl[i]->id < next->id)) next = S.cl[i];
    if (next) give(next, 0, ns, &n);
    else if (S.owner) { ns[n++] = (note){ S.owner, 0 }; restore(); }
    pthread_mutex_unlock(&S.mu);
    tell(ns, n);
}

void sdraw_take_back(void)
{
    note ns[1]; int n = 0;
    pthread_mutex_lock(&S.mu);
    if (S.owner) { ns[n++] = (note){ S.owner, 0 }; restore(); }
    pthread_mutex_unlock(&S.mu);
    tell(ns, n);
}

static void jstr(char *out, size_t n, size_t *k, const char *s)
{
    if (*k < n) out[(*k)++] = '"';
    for (; *s && *k + 7 < n; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\') { out[(*k)++] = '\\'; out[(*k)++] = (char)ch; }
        else if (ch < 0x20) *k += (size_t)snprintf(out + *k, n - *k, "\\u%04x", ch);
        else out[(*k)++] = (char)ch;
    }
    if (*k < n) out[(*k)++] = '"';
}

int sdraw_status_json(char *out, size_t n)
{
    size_t k = 0;
#define PUT(...) do { if (k < n) k += (size_t)snprintf(out + k, n - k, __VA_ARGS__); } while (0)
    pthread_mutex_lock(&S.mu);
    PUT("{\"version\":\"%s\",\"display\":{\"fd\":%d,\"fb\":%u,\"mapped\":%s,\"error\":", ADDIN_VERSION, S.fd, S.fb, S.px ? "true" : "false");
    jstr(out, n, &k, S.err);
    PUT("},\"refreshes\":{\"mpc\":%lu,\"ours\":%lu,\"ours_failed\":%lu,\"page_flips\":%lu},\"owner\":%d,\"clients\":[",
        S.mpc_dirty, S.own_dirty, S.dirty_err, S.flips, S.owner ? S.owner->id : 0);
    int first = 1;
    for (int i = 0; i < SDRAW_MAX_CLIENTS; i++) {
        if (!S.cl[i]) continue;
        PUT("%s{\"id\":%d,\"name\":", first ? "" : ",", S.cl[i]->id);
        jstr(out, n, &k, S.cl[i]->name);
        PUT(",\"has_screen\":%s}", S.owner == S.cl[i] ? "true" : "false");
        first = 0;
    }
    pthread_mutex_unlock(&S.mu);
    PUT("]}");
#undef PUT
    if (k >= n) { out[n - 1] = 0; return -1; }
    out[k] = 0;
    return 0;
}
