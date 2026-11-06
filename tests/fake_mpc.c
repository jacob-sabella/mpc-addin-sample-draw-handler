/* A stand-in for MPC on the build machine. Run as "MPC Sample" with the addin preloaded and MPC_SDRAW_TEST_FB naming a
 * 320x240x4 file that plays the framebuffer. It plays MPC (paints the file, asks for refreshes through ioctl) and two
 * plugins (clients A and B), and checks what ends up "on screen". With "wait" it keeps running for the control tests. */
#define _GNU_SOURCE
#include "../src/mpc_sample_draw.h"
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <sys/stat.h>

struct drm_clip_rect { unsigned short x1, y1, x2, y2; };
struct drm_mode_fb_dirty_cmd { unsigned fb_id, flags, color, num_clips; unsigned long long clips_ptr; };
#define DIRTYFB _IOWR('d', 0xB1, struct drm_mode_fb_dirty_cmd)
enum { W = 320, H = 240 };
static unsigned *fb; static int fails, dfd;
#define CHECK(c, msg) do { if (c) printf("ok   %s\n", msg); else { printf("FAIL %s\n", msg); fails++; } } while (0)
static int focus_a[2], focus_b[2];
static void fa(void *u, int h) { (void)u; focus_a[h]++; }
static void fb_(void *u, int h) { (void)u; focus_b[h]++; }
static void paint(int x0, int y0, int w, int h, unsigned col) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) fb[y * W + x] = col; }
static int all(int x0, int y0, int w, int h, unsigned col) { for (int y = y0; y < y0 + h; y++) for (int x = x0; x < x0 + w; x++) if (fb[y * W + x] != col) return 0; return 1; }
static void mpc_refresh(int x0, int y0, int w, int h)
{
    struct drm_clip_rect c = { x0, y0, x0 + w, y0 + h };
    struct drm_mode_fb_dirty_cmd d = { .fb_id = 1, .num_clips = 1, .clips_ptr = (unsigned long)&c };
    ioctl(dfd, DIRTYFB, &d);   /* the addin sees it; the real ioctl on a pipe fails, as expected */
}

static mpc_sdraw_event in_ev[16]; static volatile int n_in;
static void on_input(void *u, const mpc_sdraw_event *e) { (void)u; if (n_in < 16) in_ev[n_in] = *e; n_in++; }

struct drm_mode_crtc_page_flip { unsigned crtc_id, fb_id, flags, reserved; unsigned long long user_data; };
#define PAGE_FLIP _IOWR('d', 0xB0, struct drm_mode_crtc_page_flip)
static void mpc_flip(unsigned id) { struct drm_mode_crtc_page_flip pf = { .fb_id = id }; ioctl(dfd, PAGE_FLIP, &pf); }
static unsigned *map_file(const char *path) { int f = open(path, O_RDWR); return f < 0 ? NULL : mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0); }
static int has(const unsigned *b, unsigned col) { for (int i = 0; i < W * H; i++) if (b[i] == col) return 1; return 0; }

/* MPC double-buffered, as on the Sample: two framebuffers, page flips between them, and each frame redraws only what
 * changed (into the buffer going out next, which still holds the frame before last). MPC_SDRAW_TEST_FB has "%u". */
static int flip_test(const mpc_sdraw_api *A, const char *pat)
{
    char p1[512], p2[512];
    const char *pc = strstr(pat, "%u");
    snprintf(p1, sizeof p1, "%.*s1%s", (int)(pc - pat), pat, pc + 2);
    snprintf(p2, sizeof p2, "%.*s2%s", (int)(pc - pat), pat, pc + 2);
    unsigned *b1 = map_file(p1), *b2 = map_file(p2);
    if (!b1 || !b2) { printf("FAIL the two framebuffer files\n"); return 1; }
    for (int i = 0; i < W * H; i++) b1[i] = b2[i] = 0x111111;
    mpc_flip(1);                                                  /* frame 0 on buffer 1 */
    mpc_sdraw_client *c = A->open("page", NULL, NULL);
    int st; unsigned *l = A->pixels(c, &st); for (int i = 0; i < W * H; i++) l[i] = 0xAA0000;
    CHECK(A->request(c) == 0, "flip: the page gets the screen");
    fb = b2; paint(10, 10, 50, 50, 0x222222); mpc_flip(2);       /* frame 1: buffer 2 had frame 0, plus the change */
    CHECK(all(0, 0, W, H, 0xAA0000), "flip: buffer 2 goes out with the page");
    CHECK(!has(b1, 0xAA0000) && b1[0] == 0x111111, "flip: buffer 1, off screen, has MPC's frame 0 back");
    fb = b1; paint(10, 10, 50, 50, 0x222222); paint(200, 200, 50, 30, 0x333333); mpc_flip(1);   /* frame 2 */
    CHECK(!has(b2, 0xAA0000), "flip: buffer 2, off screen, has MPC's frame 1 back");
    fb = b2; paint(200, 200, 50, 30, 0x333333); paint(100, 100, 10, 10, 0x444444); mpc_flip(2); /* frame 3 */
    A->release(c);
    fb = b2;
    CHECK(!has(b1, 0xAA0000) && !has(b2, 0xAA0000), "flip: release leaves no page pixels in either buffer");
    CHECK(all(0, 0, 10, 10, 0x111111) && all(10, 10, 50, 50, 0x222222) && all(200, 200, 50, 30, 0x333333) &&
          all(100, 100, 10, 10, 0x444444), "flip: release shows MPC's latest frame whole");
    A->close(c);
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails != 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "flip")) {
        int p[2]; pipe(p); dfd = p[0];
        const mpc_sdraw_api *(*g)(int) = (const mpc_sdraw_api *(*)(int))dlsym(RTLD_DEFAULT, MPC_SDRAW_ENTRY);
        const mpc_sdraw_api *A = g ? g(MPC_SDRAW_API_VERSION) : NULL;
        return A ? flip_test(A, getenv("MPC_SDRAW_TEST_FB")) : 1;
    }
    int f = open(getenv("MPC_SDRAW_TEST_FB"), O_RDWR);
    fb = mmap(NULL, W * H * 4, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    int p[2]; pipe(p); dfd = p[0];
    const mpc_sdraw_api *(*get)(int) = (const mpc_sdraw_api *(*)(int))dlsym(RTLD_DEFAULT, MPC_SDRAW_ENTRY);
    if (argc > 1 && !strcmp(argv[1], "none")) { const mpc_sdraw_api *a = get ? get(1) : NULL; printf(a ? "FAIL api in another process\n" : "ok   no api in another process\n"); return a != NULL; }
    const mpc_sdraw_api *A = get ? get(MPC_SDRAW_API_VERSION) : NULL;
    CHECK(A && A->width == W && A->height == H, "the api is found by name, 320x240");
    if (!A) return 1;
    CHECK(get(MPC_SDRAW_API_VERSION + 1) == NULL, "a newer api version than the addin's is refused");
    paint(0, 0, W, H, 0x111111);               /* MPC's screen */
    if (argc > 1 && !strcmp(argv[1], "wait")) {   /* control tests: one client, then wait */
        mpc_sdraw_client *c = A->open("waiting plugin", NULL, NULL);
        int st; unsigned *px = A->pixels(c, &st); for (int i = 0; i < W * H; i++) px[i] = 0x00AA00;
        printf("ready\n"); fflush(stdout);
        pause();
    }
    if (argc > 1 && !strcmp(argv[1], "input")) {   /* the page button, through a FIFO standing in for the control surface */
        const char *path = getenv("MPC_SDRAW_TEST_SURFACE");
        int s = open(path, O_RDWR | O_NONBLOCK);
        mpc_sdraw_client *ca = A->open("plugin A", fa, NULL), *cb = A->open("plugin B", fb_, NULL);
        unsigned char r[16];
#define W3(a, b, c) do { unsigned char m[3] = { a, b, c }; write(s, m, 3); } while (0)
        W3(0x90, 7, 0x7F);
        CHECK(read(s, r, sizeof r) == 3 && r[1] == 7 && r[2] == 0x7F, "another button passes through");
        W3(0x90, 0, 0x7F);
        CHECK(read(s, r, sizeof r) == -1 && errno == EAGAIN, "the page button's press is held back");
        W3(0x90, 0, 0x00);
        CHECK(read(s, r, sizeof r) == 3 && r[1] == 0 && r[2] == 0x7F, "a short press reaches MPC as a press on release");
        struct pollfd pf = { .fd = s, .events = POLLIN };
        CHECK(poll(&pf, 1, 0) == 1 && (pf.revents & POLLIN), "poll says the release is waiting");
        CHECK(read(s, r, sizeof r) == 3 && r[1] == 0 && r[2] == 0, "the release comes on the next read");
        CHECK(!A->has_screen(ca) && !A->has_screen(cb), "a short press changes no screen");
        int ok = 1;
        for (int round = 0; round < 3; round++) {
            W3(0x90, 0, 0x7F); read(s, r, sizeof r);
            usleep(1100 * 1000);
            W3(0x90, 0, 0x00);
            ssize_t k = read(s, r, sizeof r);
            if (k != -1) ok = 0;
            int want_a = round == 0, want_b = round == 1;
            if (A->has_screen(ca) != want_a || A->has_screen(cb) != want_b) ok = 0;
        }
        CHECK(ok, "long holds cycle A -> B -> MPC and never reach MPC");
        CHECK(focus_a[1] == 1 && focus_a[0] == 1 && focus_b[1] == 1 && focus_b[0] == 1, "each plugin is told it got and lost the screen");
        { unsigned char m[5] = { 0x9E, 0xF8, 0x90, 0x05, 0x7F }; write(s, m, 5); }
        CHECK(read(s, r, sizeof r) == 5 && r[0] == 0x9E && r[2] == 0x90 && r[3] == 5, "a broken message passes through untouched");
        W3(0x90, 0, 0x7F); W3(0x90, 7, 0x7F);
        CHECK(read(s, r, sizeof r) == 3 && r[1] == 7, "a press in the same read as the page button still gets through");
        W3(0x90, 0, 0x00); read(s, r, sizeof r); read(s, r, sizeof r);
        /* timestamped frames, as MPC reads the Sample's port: a second fd on the same FIFO */
        int s2 = open(path, O_RDWR | O_NONBLOCK);
        unsigned char rf[128];
#define FR(o, a, b_, c) do { memset(fr + (o), 0, 32); fr[(o) + 1] = 3; fr[(o) + 4] = 0x55; fr[(o) + 8] = 0x77; \
                             fr[(o) + 16] = a; fr[(o) + 17] = b_; fr[(o) + 18] = c; } while (0)
        unsigned char fr[64];
        FR(0, 0x99, 0x24, 0x40); write(s2, fr, 32);
        CHECK(read(s2, rf, sizeof rf) == 32 && rf[16] == 0x99 && rf[4] == 0x55, "framed: a pad frame passes through whole");
        FR(0, 0x90, 0, 0x7F); write(s2, fr, 32);
        CHECK(read(s2, rf, sizeof rf) == -1 && errno == EAGAIN, "framed: the page button's press frame is held back");
        FR(0, 0x80, 0, 0x00); write(s2, fr, 32);
        CHECK(read(s2, rf, sizeof rf) == 32 && rf[16] == 0x90 && rf[18] == 0x7F, "framed: a short press reaches MPC as its press frame");
        CHECK(read(s2, rf, sizeof rf) == 32 && rf[16] == 0x80 && rf[17] == 0, "framed: then its release frame");
        FR(0, 0x90, 0, 0x7F); FR(32, 0x99, 0x25, 0x10); write(s2, fr, 64);
        CHECK(read(s2, rf, sizeof rf) == 32 && rf[16] == 0x99 && rf[17] == 0x25, "framed: a frame read with the page button still gets through");
        usleep(1100 * 1000);
        FR(0, 0x80, 0, 0x00); write(s2, fr, 32);
        CHECK(read(s2, rf, sizeof rf) == -1 && A->has_screen(ca), "framed: a long hold cycles the screen and never reaches MPC");
        { unsigned char t[32] = { 0 }; t[1] = 3; t[4] = 0x90; t[5] = 0x00; t[6] = 0x7F; t[16] = 0xB0; t[17] = 0x64; t[18] = 0x01;
          write(s2, t, 32); }
        CHECK(read(s2, rf, sizeof rf) == 32 && A->has_screen(ca), "framed: a timestamp that looks like the page button is ignored");
        /* API 2: the client with the screen takes the soft keys, the dial and the knobs */
        CHECK(A->version >= 2 && A->set_input, "api version 2 has set_input");
        A->set_input(ca, on_input, NULL);   /* ca has the screen (the long hold above) */
        FR(0, 0x90, 12, 0x7F); write(s2, fr, 32);
        CHECK(read(s2, rf, sizeof rf) == -1 && errno == EAGAIN, "framed: a soft key goes to the client, not MPC");
        FR(0, 0xB0, 100, 0x7F); FR(32, 0x99, 0x24, 0x30); write(s2, fr, 64);
        CHECK(read(s2, rf, sizeof rf) == 32 && rf[16] == 0x99, "framed: the dial is taken, a pad in the same read still reaches MPC");
        { unsigned char t[32] = { 0 }; t[1] = 6; t[16] = 0xB0; t[17] = 17; t[18] = 90; t[19] = 0xB0; t[20] = 30; t[21] = 5;
          write(s2, t, 32); }
        CHECK(read(s2, rf, sizeof rf) == 32 && rf[1] == 3 && rf[16] == 0xB0 && rf[17] == 30 && rf[19] == 0,
              "framed: a knob is cut out of a frame, the other control in it stays");
        usleep(150 * 1000);
        CHECK(n_in == 3 && in_ev[0].type == MPC_SDRAW_EV_KEY && in_ev[0].id == 0 && in_ev[0].value == 1 &&
              in_ev[1].type == MPC_SDRAW_EV_DIAL && in_ev[1].value == -1 &&
              in_ev[2].type == MPC_SDRAW_EV_KNOB && in_ev[2].id == 1 && in_ev[2].value == 90, "the client got key, dial and knob events");
        A->release(ca);
        n_in = 0;
        CHECK(A->request(ca) == 0, "the client asks for the screen again");
        usleep(150 * 1000);
        CHECK(n_in == 1 && in_ev[0].type == MPC_SDRAW_EV_KNOB_AT && in_ev[0].id == 1 && in_ev[0].value == 90,
              "getting the screen, it is told where the knobs already are");
        n_in = 0;
        FR(0, 0x90, 111, 0x7F); write(s2, fr, 32);
        CHECK(read(s2, rf, sizeof rf) == -1 && errno == EAGAIN, "framed: the dial press goes to the client, not MPC");
        usleep(150 * 1000);
        CHECK(n_in == 1 && in_ev[0].type == MPC_SDRAW_EV_KEY && in_ev[0].id == 3 && in_ev[0].value == 1, "the dial press is key 3");
        /* API 3: show hands the screen to a client by name; the page button then goes back */
        CHECK(A->version >= 3 && A->show, "api version 3 has show");
        CHECK(A->show(ca, "no such plugin") == -1 && A->has_screen(ca), "show of an unknown name fails, the screen stays");
        mpc_sdraw_client *cc = A->open("plugin C", fa, NULL);
        CHECK(A->show(ca, "plugin C") == 0 && A->has_screen(cc) && !A->has_screen(ca), "A shows C: C has the screen");
        FR(0, 0x90, 0, 0x7F); write(s2, fr, 32); read(s2, rf, sizeof rf); usleep(1100 * 1000);
        FR(0, 0x80, 0, 0x00); write(s2, fr, 32); read(s2, rf, sizeof rf);
        CHECK(A->has_screen(ca), "the page button on C goes back to A, not on to MPC");
        FR(0, 0x90, 0, 0x7F); write(s2, fr, 32); read(s2, rf, sizeof rf); usleep(1100 * 1000);
        FR(0, 0x80, 0, 0x00); write(s2, fr, 32); read(s2, rf, sizeof rf);
        CHECK(A->has_screen(cb), "from A it cycles on as usual (to B)");
        A->close(cc);
        A->release(cb);
        A->request(ca);
        A->release(ca);
        FR(0, 0x90, 12, 0x7F); write(s2, fr, 32);
        CHECK(read(s2, rf, sizeof rf) == 32 && rf[17] == 12, "without the screen, the soft key goes to MPC");
        /* with the home page (RACK) open, the page button toggles it, and a page it showed goes back to it */
        A->release(ca); A->release(cb);
        mpc_sdraw_client *rk = A->open("RACK", fa, NULL);
#define HOLD() do { FR(0, 0x90, 0, 0x7F); write(s2, fr, 32); read(s2, rf, sizeof rf); usleep(1100 * 1000); \
                    FR(0, 0x80, 0, 0x00); write(s2, fr, 32); read(s2, rf, sizeof rf); } while (0)
        HOLD();
        CHECK(A->has_screen(rk), "home: a hold from MPC opens RACK");
        HOLD();
        CHECK(!A->has_screen(rk) && !A->has_screen(ca) && !A->has_screen(cb), "home: a hold on RACK closes it, no cycling");
        HOLD();
        CHECK(A->show(rk, "plugin B") == 0 && A->has_screen(cb), "home: RACK shows plugin B");
        HOLD();
        CHECK(A->has_screen(rk), "home: a hold on plugin B goes back to RACK");
        A->release(rk);
        A->request(ca);
        HOLD();
        CHECK(!A->has_screen(ca) && !A->has_screen(rk), "home: a hold on a page RACK didn't open closes it");
        A->close(rk);
        close(s2);
        A->close(ca); A->close(cb);
        printf(fails ? "FAILED\n" : "PASSED\n");
        return fails != 0;
    }
    mpc_sdraw_client *a = A->open("plugin A", fa, NULL), *b = A->open("plugin B", fb_, NULL);
    int st; unsigned *la = A->pixels(a, &st), *lb = A->pixels(b, &st);
    CHECK(st == W, "layer stride is the width");
    for (int i = 0; i < W * H; i++) { la[i] = 0xAA0000; lb[i] = 0x0000BB; }
    A->present(a, 0, 0, 0, 0);
    CHECK(all(0, 0, W, H, 0x111111), "present without the screen changes nothing");
    CHECK(A->request(a) == 0 && A->has_screen(a), "A asks and gets the screen");
    CHECK(all(0, 0, W, H, 0xAA0000), "A's layer is on screen");
    CHECK(!focus_a[0] && !focus_a[1], "no callback for A's own request");
    paint(10, 10, 50, 50, 0x222222); mpc_refresh(10, 10, 50, 50);
    CHECK(all(10, 10, 50, 50, 0xAA0000), "MPC's refresh of a rectangle shows A's pixels there, not MPC's");
    for (int y = 100; y < 120; y++) for (int x = 100; x < 140; x++) la[y * W + x] = 0xFFFFFF;
    A->present(a, 100, 100, 40, 20);
    CHECK(all(100, 100, 40, 20, 0xFFFFFF) && all(0, 0, 50, 5, 0xAA0000), "A's present shows its rectangle");
    CHECK(A->request(b) == 0 && A->has_screen(b) && !A->has_screen(a), "B asks: the last to ask gets the screen");
    CHECK(focus_a[0] == 1 && !focus_b[1], "A is told it lost the screen; B gets no callback for its own request");
    CHECK(all(0, 0, W, H, 0x0000BB), "B's layer is on screen");
    A->present(a, 0, 0, 0, 0);
    CHECK(all(0, 0, W, H, 0x0000BB), "A can't draw once it lost the screen");
    A->release(a);
    CHECK(A->has_screen(b), "A's release doesn't touch B's screen");
    A->release(b);
    CHECK(!A->has_screen(b) && all(0, 0, 10, 10, 0x111111) && all(10, 10, 50, 50, 0x222222) && all(200, 200, 50, 30, 0x111111),
          "B releases: MPC's screen is back, with what MPC drew meanwhile");
    CHECK(!focus_b[0], "no callback for B's own release");
    CHECK(A->request(a) == 0, "A asks again (nothing is handed over by itself)");
    A->close(a);
    CHECK(all(0, 0, 10, 10, 0x111111), "closing the owner gives MPC its screen back");
    int n = 0; ioctl(p[1], FIONREAD, &n);   /* not DRM: straight through */
    CHECK(ioctl(p[0], FIONREAD, &n) == 0, "other ioctls pass through");
    A->close(b);
    printf(fails ? "FAILED\n" : "PASSED\n");
    return fails != 0;
}
