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

int main(int argc, char **argv)
{
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
