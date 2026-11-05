/* mpc-addin-sample-draw-handler, the page button: MPC reads the Sample's buttons from the control-surface rawmidi port
 * (/dev/snd/midiC0D0: note on 0x90 <note> 0x7F on press, note off 0x80 <note> 0x00 on release, channel 1; PAD FX is note 1,
 * KNOB FX note 0). MPC opens the port in ALSA's timestamped framing mode: each read is whole 32-byte frames (frame type,
 * length, 2 reserved, tv_nsec, tv_sec, then up to 16 MIDI bytes), and a button is one frame. Plain MIDI streams (the host
 * tests, or a version that doesn't frame) are handled too. The addin wraps read()
 * and poll()/ppoll() on that fd. A long hold of the page button (page_note, page_hold_ms) cycles the screen: MPC -> the
 * first client -> the next -> ... -> MPC. The hold never reaches MPC; a short press reaches MPC as press and release when
 * the button is let go (MPC acts on the first message of a read, so the release is handed over by a second read, which
 * poll() is told about). */
#define _GNU_SOURCE
#include "sdraw_int.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static ssize_t (*real_read)(int, void *, size_t);
static int (*real_poll)(struct pollfd *, nfds_t, int);
static int (*real_ppoll)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);

static int page_note = 1, hold_ms = 800, log_buttons = 1;
static atomic_int on;

#define MAXFD 1024
static unsigned char cls[MAXFD];   /* 0 unknown, 1 the control surface, 2 not */
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned char pend[64]; static int npend, pend_fd = -1;   /* bytes to hand to MPC on its next read */
static int held; static long long held_since; static int fired;
static unsigned char run_status;   /* running status of the stream */
static unsigned char framed[MAXFD];   /* the fd has been seen to deliver timestamped frames */
static unsigned char down_frame[32];  /* the page button's held-back press frame */

static long long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000LL + t.tv_nsec / 1000000; }

void input_config(int note, int ms, int log) { page_note = note; hold_ms = ms; log_buttons = log; }

/* The controls the client with the screen can take (see mpc_sample_draw.h's events), and the events waiting for the
 * delivery thread (taken under mu, handed over outside it). */
static int key_note[4] = { -1, -1, -1, -1 }, knob_cc[3] = { -1, -1, -1 }, dial_cc = -1;
void input_controls(const int keys[4], const int knobs[3], int dial)
{
    for (int i = 0; i < 4; i++) key_note[i] = keys[i];
    for (int i = 0; i < 3; i++) knob_cc[i] = knobs[i];
    dial_cc = dial;
}
static pthread_cond_t ev_cond = PTHREAD_COND_INITIALIZER;
static mpc_sdraw_event evq[64]; static int nev;
static int knob_at[3] = { -1, -1, -1 };   /* every knob's last position, seen whoever has the screen */

/* The client taking input changed (lock held): tell the new one where the knobs are, before anything else it gets. */
static int last_owner; static unsigned last_gen;
static void owner_check(int owner, unsigned gen)
{
    if (owner && (owner != last_owner || gen != last_gen))
        for (int i = 0; i < 3 && nev < (int)(sizeof evq / sizeof *evq); i++)
            if (knob_at[i] >= 0) { evq[nev++] = (mpc_sdraw_event){ MPC_SDRAW_EV_KNOB_AT, i, knob_at[i] }; pthread_cond_signal(&ev_cond); }
    last_owner = owner; last_gen = gen;
}

/* A channel-1 control change for a knob: remember where it is (lock held). */
static void see_knob(const unsigned char *m)
{
    if (m[0] != 0xB0) return;
    for (int i = 0; i < 3; i++) if (knob_cc[i] >= 0 && m[1] == knob_cc[i]) knob_at[i] = m[2];
}

/* A whole channel-1 message (3 bytes) for a taken control while the client with the screen wants them (lock held):
 * queued as an event, and 1 so the caller drops it from what MPC reads. */
static int take(const unsigned char *m)
{
    if ((m[0] & 15) != 0) return 0;
    unsigned gen; int owner = sdraw_input_owner(&gen);
    owner_check(owner, gen);   /* before this message changes knob_at: the report is where the knob was */
    see_knob(m);
    if (!owner) return 0;
    mpc_sdraw_event e = { 0, 0, 0 };
    int kind = m[0] & 0xF0;
    if (kind == 0x90 || kind == 0x80) {
        for (int i = 0; i < 4; i++)   /* 3: the dial pressed in */
            if (key_note[i] >= 0 && m[1] == key_note[i]) { e.type = MPC_SDRAW_EV_KEY; e.id = i; e.value = kind == 0x90 && m[2]; }
    } else if (kind == 0xB0) {
        if (dial_cc >= 0 && m[1] == dial_cc) { e.type = MPC_SDRAW_EV_DIAL; e.value = m[2] < 64 ? m[2] : m[2] - 128; }
        for (int i = 0; i < 3; i++)
            if (knob_cc[i] >= 0 && m[1] == knob_cc[i]) { e.type = MPC_SDRAW_EV_KNOB; e.id = i; e.value = m[2]; }
    }
    if (!e.type) return 0;
    if (nev < (int)(sizeof evq / sizeof *evq)) { evq[nev++] = e; pthread_cond_signal(&ev_cond); }
    return 1;
}

static int is_surface(int fd)
{
    if (fd < 0 || fd >= MAXFD) return 0;
    if (cls[fd]) return cls[fd] == 1;
    char p[64], t[128];
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, t, sizeof t - 1);
    if (n <= 0) return 0;
    t[n] = 0;
    int test = getenv("MPC_SDRAW_TEST_SURFACE") && strstr(t, getenv("MPC_SDRAW_TEST_SURFACE"));
    cls[fd] = (strstr(t, "/dev/snd/midiC0D0") || test) ? 1 : 2;
    return cls[fd] == 1;
}

/* A closed fd forgets what it was, so a number reused for another file is checked again. */
__attribute__((visibility("default"))) int close(int fd)
{
    static int (*real_close)(int);
    if (!real_close) real_close = (int (*)(int))dlsym(RTLD_NEXT, "close");
    if (fd >= 0 && fd < MAXFD) cls[fd] = framed[fd] = 0;
    return real_close(fd);
}

static void *hold_thread(void *arg);

static void resolve(void)
{
    if (!real_read) {
        real_poll = (int (*)(struct pollfd *, nfds_t, int))dlsym(RTLD_NEXT, "poll");
        real_ppoll = (int (*)(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *))dlsym(RTLD_NEXT, "ppoll");
        __atomic_store_n(&real_read, (ssize_t (*)(int, void *, size_t))dlsym(RTLD_NEXT, "read"), __ATOMIC_RELEASE);
    }
}

void input_enable(void)
{
    resolve();
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    pthread_t t;
    if (pthread_create(&t, NULL, hold_thread, NULL) == 0) pthread_detach(t);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    atomic_store(&on, 1);
}

/* The page button went down (lock held): hold it back. */
static void page_down(void) { held = 1; held_since = now_ms(); fired = 0; }

/* Watches a held page button: past page_hold_ms it fires (outside MPC's threads). */
static void *hold_thread(void *arg)
{
    (void)arg;
    for (;;) {
        mpc_sdraw_event got[64]; int ng = 0, fire = 0;
        pthread_mutex_lock(&mu);
        if (!nev) {
            struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 30 * 1000000L; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
            pthread_cond_timedwait(&ev_cond, &mu, &ts);
        }
        { unsigned gen; int o = sdraw_input_owner(&gen); owner_check(o, gen); }   /* also when no control moves (lock order mu -> the core's, as in take) */
        ng = nev; memcpy(got, evq, (size_t)nev * sizeof *evq); nev = 0;
        if (held && !fired && now_ms() - held_since >= hold_ms) { fired = 1; fire = 1; }
        pthread_mutex_unlock(&mu);
        for (int i = 0; i < ng; i++) sdraw_deliver(&got[i]);
        if (fire) { if (log_buttons) LOG("page button held: cycling the screen\n"); sdraw_cycle(); }
    }
    return NULL;
}

#define FRAME 32
#define FRAME_DATA 16
/* Whole frames of type 0 (the only one), each 1..16 MIDI bytes with zero reserved bytes. */
static int looks_framed(const unsigned char *b, ssize_t n)
{
    if (n < FRAME || n % FRAME) return 0;
    for (ssize_t f = 0; f < n; f += FRAME)
        if (b[f] != 0 || b[f + 1] < 1 || b[f + 1] > FRAME_DATA || b[f + 2] || b[f + 3]) return 0;
    return 1;
}

static void log_notes(const unsigned char *d, int len)
{
    for (int j = 0; j + 3 <= len; ) {
        unsigned char st = d[j];
        if ((st & 0xE0) == 0x80 && !(d[j + 1] & 0x80) && !(d[j + 2] & 0x80)) {
            if ((st & 15) == 0 || log_buttons >= 2)   /* channel 1 is the buttons; the pads (channel 10) only at 2 */
                LOG("button: note %d %s (ch %d)\n", d[j + 1], (st & 0xF0) == 0x90 && d[j + 2] ? "down" : "up", (st & 15) + 1);
            j += 3;
        } else if (st == 0xB0 && !(d[j + 1] & 0x80) && !(d[j + 2] & 0x80)) {   /* channel 1 controls (not the dial) */
            if (d[j + 1] != dial_cc || log_buttons >= 2) LOG("control: cc %d = %d (ch 1)\n", d[j + 1], d[j + 2]);
            j += 3;
        } else j++;
    }
}

/* Framed reads: the page button is a frame of its own (3 bytes: 9x/8x <note> <vel> on channel 1). Its press frame is held
 * back; a long hold's release is dropped; a short press hands MPC the press frame when the button is let go and queues
 * the release frame for the next read. Every other frame passes untouched. */
static ssize_t filter_framed(int fd, unsigned char *b, ssize_t n)
{
    ssize_t o = 0;
    for (ssize_t f = 0; f + FRAME <= n; f += FRAME) {
        unsigned char *fr = b + f, *d = fr + FRAME_DATA;
        int len = fr[1];
        if (log_buttons) log_notes(d, len);
        if (len > FRAME_DATA) len = FRAME_DATA;
        {   /* controls the client with the screen takes: out of the frame (a frame left empty goes) */
            int w = 0, j = 0, cut = 0;
            while (j < len) {
                if (j + 3 <= len && (d[j] & 0x80) && !(d[j + 1] & 0x80) && !(d[j + 2] & 0x80) && take(d + j)) { j += 3; cut = 1; continue; }
                d[w++] = d[j++];
            }
            if (cut) {
                if (!w) continue;
                fr[1] = (unsigned char)w; len = w;
                memset(d + w, 0, (size_t)(FRAME_DATA - w));
            }
        }
        if (len == 3 && (d[0] == 0x90 || d[0] == 0x80) && d[1] == page_note) {
            int down = d[0] == 0x90 && d[2];
            if (down) { memcpy(down_frame, fr, FRAME); page_down(); continue; }
            if (held) {
                held = 0;
                if (!fired) {
                    if (npend + FRAME <= (int)sizeof pend) { memcpy(pend + npend, fr, FRAME); npend += FRAME; pend_fd = fd; }
                    memcpy(b + o, down_frame, FRAME); o += FRAME;
                }
                continue;
            }
        }
        if (o != f) memmove(b + o, fr, FRAME);
        o += FRAME;
    }
    return o;
}

/* Filter what MPC just read from the control surface (lock held): drop the page button's hold, turn a short press into
 * press now + release on the next read. Returns the new length. */
static ssize_t filter(int fd, unsigned char *b, ssize_t n)
{
    if (log_buttons >= 2) {   /* the raw bytes of each read, to see what the control surface really sends */
        char hx[3 * 64 + 1]; int k = 0;
        for (ssize_t j = 0; j < n && j < 64; j++) k += snprintf(hx + k, sizeof hx - (size_t)k, "%02x ", b[j]);
        LOG("raw %zd: %s\n", n, hx);
    }
    if (fd >= 0 && fd < MAXFD && (framed[fd] || looks_framed(b, n))) { framed[fd] = 1; return filter_framed(fd, b, n); }
    ssize_t o = 0;
    for (ssize_t i = 0; i < n;) {
        unsigned char st = b[i];
        int len;
        if (st >= 0x80) { len = st >= 0xF0 ? (st == 0xF0 ? -1 : (st == 0xF1 || st == 0xF3) ? 2 : st == 0xF2 ? 3 : 1)
                                            : (st >= 0xC0 && st < 0xE0) ? 2 : 3;
                          if (st < 0xF0) run_status = st; }
        else len = 0;   /* running status or SysEx data: pass through untouched */
        if (len == -1 || len == 0) { b[o++] = b[i++]; continue; }   /* SysEx/data bytes: copied one by one */
        if (i + len > n) { while (i < n) b[o++] = b[i++]; break; }  /* a message split across reads: let it through */
        int stray = 0;   /* a status byte where data should be: not a whole message; pass the status on and resync */
        for (int j = 1; j < len; j++) if (b[i + j] & 0x80) stray = 1;
        if (stray) { b[o++] = b[i++]; continue; }
        if (len == 3 && take(b + i)) { i += len; continue; }
        if ((st & 0xF0) == 0x90 || (st & 0xF0) == 0x80) {
            int note = b[i + 1], vel = (st & 0xF0) == 0x90 ? b[i + 2] : 0;
            if (log_buttons) LOG("button: note %d %s (ch %d)\n", note, vel ? "down" : "up", (st & 15) + 1);
            if (note == page_note && (st & 15) == 0) {
                if (vel) { page_down(); i += len; continue; }
                if (held) {
                    held = 0;
                    if (!fired) {   /* a short press: MPC gets press now, release on its next read */
                        b[o++] = 0x90; b[o++] = (unsigned char)note; b[o++] = 0x7F;
                        if (npend + 3 <= (int)sizeof pend) { pend[npend++] = 0x90; pend[npend++] = (unsigned char)note; pend[npend++] = 0; pend_fd = fd; }
                    }
                    i += len;
                    continue;
                }
            }
        }
        memcpy(b + o, b + i, (size_t)len); o += len; i += len;
    }
    return o;
}

__attribute__((visibility("default"))) ssize_t read(int fd, void *buf, size_t n)
{
    resolve();
    if (!atomic_load_explicit(&on, memory_order_relaxed) || !is_surface(fd) || n == 0) return real_read(fd, buf, n);
    for (;;) {
        pthread_mutex_lock(&mu);
        if (npend && pend_fd == fd) {   /* a held-back release first, one message (or frame) per read */
            size_t k = (size_t)npend < n ? (size_t)npend : n;
            memcpy(buf, pend, k);
            memmove(pend, pend + k, (size_t)npend - k); npend -= (int)k;
            pthread_mutex_unlock(&mu);
            return (ssize_t)k;
        }
        pthread_mutex_unlock(&mu);
        ssize_t r = real_read(fd, buf, n);
        if (r <= 0) return r;
        pthread_mutex_lock(&mu);
        ssize_t k = filter(fd, buf, r);
        pthread_mutex_unlock(&mu);
        if (k > 0) return k;
        /* everything was held back: never hand MPC 0 (end of file) */
        if (fcntl(fd, F_GETFL) & O_NONBLOCK) { errno = EAGAIN; return -1; }
    }
}

/* poll: a held-back release makes the fd readable */
static int mark(struct pollfd *fds, nfds_t nf, int r)
{
    if (!atomic_load_explicit(&on, memory_order_relaxed)) return r;
    pthread_mutex_lock(&mu);
    int has = npend > 0 ? pend_fd : -1;
    pthread_mutex_unlock(&mu);
    if (has < 0) return r;
    for (nfds_t i = 0; i < nf; i++)
        if (fds[i].fd == has && (fds[i].events & POLLIN) && !(fds[i].revents & POLLIN)) {
            fds[i].revents |= POLLIN;
            r = r < 0 ? 1 : r + 1;
        }
    return r;
}

static int pending(void) { pthread_mutex_lock(&mu); int p = npend > 0; pthread_mutex_unlock(&mu); return p; }

__attribute__((visibility("default"))) int poll(struct pollfd *fds, nfds_t nf, int to)
{
    resolve();
    if (atomic_load_explicit(&on, memory_order_relaxed) && pending()) to = 0;
    return mark(fds, nf, real_poll(fds, nf, to));
}

__attribute__((visibility("default"))) int ppoll(struct pollfd *fds, nfds_t nf, const struct timespec *ts, const sigset_t *ss)
{
    resolve();
    struct timespec zero = { 0, 0 };
    if (atomic_load_explicit(&on, memory_order_relaxed) && pending()) ts = &zero;
    return mark(fds, nf, real_ppoll(fds, nf, ts, ss));
}
