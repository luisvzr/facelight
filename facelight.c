// facelight: your screen is the ring light.
//
// A Wayland take on the macOS "Edge Light" (Apple calls it "Ring Light"
// internally). The geometry comes from reverse engineering macOS's
// RingLightHelper (see docs/macos-internals.md):
//
// - ring thickness = w*90 + 10 pt, corner radius = thickness + 114
// - inner glow w*60 + 30
// - soft hole around the cursor: 2 concentric shapes (300 pt blur 60, 480 pt blur 120)
// - clicks pass through, 0 -> 1 fade when it appears
//
// One process, no toolkit: it watches /dev/video* with inotify and, while a
// camera is open, shows one wlr-layer-shell surface per output backed by an
// shm buffer. The ring is computed once; when the cursor moves only the hole's
// box is rewritten and damaged.
//
// Usage: facelight [daemon options]   run the daemon
//        facelight toggle             turn on/off by hand (back to auto when the camera closes)
//        facelight auto               drop the manual override
//        facelight status [--follow]  print {"alt": "on"|"off"|"stopped"} (once, or on every change)
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define HOLE_INNER_SIDE 300.0
#define HOLE_INNER_BLUR 60.0
#define HOLE_OUTER_SIDE 480.0
#define HOLE_OUTER_BLUR 120.0
#define HOLE_SIZE 720 // side of the hole's box: 480 + 2*120 pt
#define FADE_MS 300
#define TICK_MS 16
#define SETTLE_MS 1000 // browsers open and close /dev/video* while enumerating devices
#define FALLBACK_S 10

enum override { AUTO, FORCE_ON, FORCE_OFF };

static struct {
    bool cursor_hole;
    enum override mode;
    double width, brightness, r, g, b;
} opt = {.cursor_hole = true, .width = 0.5, .brightness = 0.95, .r = 1, .g = 0xf4 / 255.0, .b = 0xe5 / 255.0};

struct buffer {
    struct wl_buffer *wl;
    uint32_t *data;
    bool busy;
    int alpha;  // fade alpha it was painted with (0..256), -1 = never
    int box[4]; // hole box painted into this buffer (x0,y0,x1,y1), empty if x1<=x0
};

struct output {
    struct wl_list link;
    uint32_t name;
    struct wl_output *wl;
    int x, y, scale;
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    int w, h, pw, ph; // logical and pixel size
    void *pool_mem;
    size_t pool_size;
    struct buffer buf[2];
    uint32_t *base; // premultiplied ring at full alpha
    uint16_t *keep; // hole mask: 256 = keep the ring, 0 = erase it
    int hole_px;
    bool has_mouse;
    int mx, my; // cursor in buffer pixels
    int shown_box[4];
    bool dirty;
};

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_list outputs;

static bool visible, camera_on, want_shown;
static double fade_from, fade_to = 0;
static double fade_start;
static int alpha_now; // 0..256
static char hypr_sock[108], run_dir[256];
static int tick_fd, settle_fd, fallback_fd, ino_fd, sig_fd;

static double gauss(double t) { return exp(-0.5 * (t * 2.5) * (t * 2.5)); }

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void arm(int fd, int first_ms, int interval_ms) {
    struct itimerspec its = {
        .it_value = {first_ms / 1000, (first_ms % 1000) * 1000000L},
        .it_interval = {interval_ms / 1000, (interval_ms % 1000) * 1000000L},
    };
    timerfd_settime(fd, 0, &its, NULL);
}

// --- runtime files ($XDG_RUNTIME_DIR/facelight/{pid,state}) -----------------

static void runtime_path(char *out, size_t n, const char *file) { snprintf(out, n, "%s/%s", run_dir, file); }

static pid_t daemon_pid(void) {
    char path[300], comm[64] = "";
    runtime_path(path, sizeof path, "pid");
    FILE *f = fopen(path, "r");
    int pid = 0;
    if (!f) return 0;
    if (fscanf(f, "%d", &pid) != 1) pid = 0;
    fclose(f);
    if (pid <= 0) return 0;
    snprintf(path, sizeof path, "/proc/%d/comm", pid);
    f = fopen(path, "r");
    if (!f) return 0;
    if (!fgets(comm, sizeof comm, f)) comm[0] = 0;
    fclose(f);
    return strncmp(comm, "facelight", 9) == 0 ? pid : 0;
}

static void write_file(const char *name, const char *text) {
    char path[300], tmp[310];
    runtime_path(path, sizeof path, name);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fputs(text, f);
    fclose(f);
    rename(tmp, path); // atomic, so `status --follow` never reads half a file
}

// --- camera ------------------------------------------------------------------

static bool camera_in_use(void) {
    DIR *proc = opendir("/proc");
    if (!proc) return false;
    struct dirent *p;
    bool found = false;
    char path[600], target[64];
    while (!found && (p = readdir(proc))) {
        if (p->d_name[0] < '0' || p->d_name[0] > '9') continue;
        snprintf(path, sizeof path, "/proc/%s/fd", p->d_name);
        DIR *fds = opendir(path);
        if (!fds) continue;
        struct dirent *f;
        while ((f = readdir(fds))) {
            snprintf(path, sizeof path, "/proc/%s/fd/%s", p->d_name, f->d_name);
            ssize_t n = readlink(path, target, sizeof target - 1);
            if (n > 0) {
                target[n] = 0;
                if (!strncmp(target, "/dev/video", 10)) { found = true; break; }
            }
        }
        closedir(fds);
    }
    closedir(proc);
    return found;
}

static void add_watches(void) {
    glob_t g;
    if (glob("/dev/video*", 0, NULL, &g)) return;
    for (size_t i = 0; i < g.gl_pathc; i++)
        inotify_add_watch(ino_fd, g.gl_pathv[i], IN_OPEN | IN_CLOSE);
    globfree(&g);
}

// --- cursor (Hyprland IPC socket) ----------------------------------------------

static bool hypr_cursor(int *x, int *y) {
    if (!hypr_sock[0]) return false;
    int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (s < 0) return false;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof addr.sun_path, "%s", hypr_sock);
    char buf[64];
    bool ok = false;
    if (connect(s, (struct sockaddr *)&addr, sizeof addr) == 0 && write(s, "cursorpos", 9) == 9) {
        ssize_t n = read(s, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            ok = sscanf(buf, "%d, %d", x, y) == 2;
        }
    }
    close(s);
    return ok;
}

// --- render -------------------------------------------------------------------

static inline uint32_t mul(uint32_t p, uint32_t k) {
    uint32_t rb = ((p & 0x00ff00ffu) * k >> 8) & 0x00ff00ffu;
    uint32_t ag = ((p >> 8) & 0x00ff00ffu) * k & 0xff00ff00u;
    return rb | ag;
}

// Ring geometry (RingLightLayer.layoutSublayers on macOS) as an SDF of the
// inner rounded rect: solid band outside it, gaussian glow inside.
static void render_base(struct output *o) {
    double thick = opt.width * 90 + 10, radius = thick + 114, glow = opt.width * 60 + 30;
    double hx = o->w / 2.0 - thick, hy = o->h / 2.0 - thick, r = radius - thick;
    for (int py = 0; py < o->ph; py++) {
        double y = (py + 0.5) / o->scale;
        double qy = fabs(y - o->h / 2.0) - (hy - r);
        for (int px = 0; px < o->pw; px++) {
            double x = (px + 0.5) / o->scale;
            double qx = fabs(x - o->w / 2.0) - (hx - r);
            double ox = qx > 0 ? qx : 0, oy = qy > 0 ? qy : 0;
            double d = sqrt(ox * ox + oy * oy) + fmin(fmax(qx, qy), 0) - r;
            double band = fmin(fmax(d * o->scale + 0.5, 0), 1);
            double g = d < 0 && -d < glow ? 0.55 * gauss(-d / glow) : 0;
            double a = opt.brightness * (band + (1 - band) * g);
            uint32_t A = (uint32_t)(a * 255 + 0.5);
            o->base[py * o->pw + px] = A << 24 | (uint32_t)(opt.r * A + 0.5) << 16 |
                                       (uint32_t)(opt.g * A + 0.5) << 8 | (uint32_t)(opt.b * A + 0.5);
        }
    }
}

// The macOS Mask layer with its 2 blurred shapes, approximated with radial gradients.
static double hole_layer(double dist, double side, double blur) {
    double R = side / 2 + blur, s0 = fmax(side / 2 - blur, 0) / R, u = dist / R;
    if (u <= s0) return 1;
    if (u >= 1) return 0;
    double t = (u - s0) / (1 - s0);
    return gauss(t) * (1 - t);
}

static void render_keep(struct output *o) {
    int n = o->hole_px = HOLE_SIZE * o->scale;
    double c = n / 2.0;
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) {
            double dist = hypot(x + 0.5 - c, y + 0.5 - c) / o->scale;
            double a1 = hole_layer(dist, HOLE_INNER_SIDE, HOLE_INNER_BLUR);
            double a2 = hole_layer(dist, HOLE_OUTER_SIDE, HOLE_OUTER_BLUR);
            o->keep[y * n + x] = (uint16_t)((1 - (a2 + a1 * (1 - a2))) * 256 + 0.5);
        }
}

static void hole_box(struct output *o, int box[4]) {
    if (!o->has_mouse || !opt.cursor_hole) { box[0] = box[1] = box[2] = box[3] = 0; return; }
    int half = o->hole_px / 2;
    box[0] = o->mx - half < 0 ? 0 : o->mx - half;
    box[1] = o->my - half < 0 ? 0 : o->my - half;
    box[2] = o->mx + half > o->pw ? o->pw : o->mx + half;
    box[3] = o->my + half > o->ph ? o->ph : o->my + half;
}

static void copy_rect(struct output *o, uint32_t *dst, const int b[4], int alpha) {
    for (int y = b[1]; y < b[3]; y++) {
        uint32_t *d = dst + y * o->pw, *s = o->base + y * o->pw;
        if (alpha >= 256) memcpy(d + b[0], s + b[0], (b[2] - b[0]) * 4);
        else for (int x = b[0]; x < b[2]; x++) d[x] = mul(s[x], alpha);
    }
}

static void buffer_release(void *data, struct wl_buffer *wl) {
    (void)wl;
    ((struct buffer *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = {.release = buffer_release};

static void render(struct output *o) {
    if (!o->base) return;
    struct buffer *b = !o->buf[0].busy ? &o->buf[0] : !o->buf[1].busy ? &o->buf[1] : NULL;
    if (!b) return; // retried on the next tick
    int box[4], full[4] = {0, 0, o->pw, o->ph};
    hole_box(o, box);
    bool all = b->alpha != alpha_now;
    copy_rect(o, b->data, all ? full : b->box, alpha_now);
    for (int y = box[1]; y < box[3]; y++) {
        uint32_t *d = b->data + y * o->pw, *s = o->base + y * o->pw;
        uint16_t *k = o->keep + (y - (o->my - o->hole_px / 2)) * o->hole_px - (o->mx - o->hole_px / 2);
        for (int x = box[0]; x < box[2]; x++) d[x] = mul(s[x], k[x] * alpha_now >> 8);
    }
    b->alpha = alpha_now;
    memcpy(b->box, box, sizeof box);

    wl_surface_attach(o->surface, b->wl, 0, 0);
    if (all) {
        wl_surface_damage_buffer(o->surface, 0, 0, o->pw, o->ph);
    } else {
        int *s = o->shown_box;
        if (s[2] > s[0]) wl_surface_damage_buffer(o->surface, s[0], s[1], s[2] - s[0], s[3] - s[1]);
        if (box[2] > box[0]) wl_surface_damage_buffer(o->surface, box[0], box[1], box[2] - box[0], box[3] - box[1]);
    }
    memcpy(o->shown_box, box, sizeof box);
    wl_surface_commit(o->surface);
    b->busy = true;
    o->dirty = false;
}

// --- surfaces -------------------------------------------------------------------

static void free_buffers(struct output *o) {
    for (int i = 0; i < 2; i++) {
        if (o->buf[i].wl) wl_buffer_destroy(o->buf[i].wl);
        o->buf[i] = (struct buffer){0};
    }
    if (o->pool_mem) munmap(o->pool_mem, o->pool_size);
    o->pool_mem = NULL;
    free(o->base);
    free(o->keep);
    o->base = NULL;
    o->keep = NULL;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *l, uint32_t serial, uint32_t w, uint32_t h) {
    struct output *o = data;
    zwlr_layer_surface_v1_ack_configure(l, serial);
    if ((int)w == o->w && (int)h == o->h && o->base) return;
    free_buffers(o);
    o->w = w;
    o->h = h;
    o->pw = w * o->scale;
    o->ph = h * o->scale;
    size_t one = (size_t)o->pw * o->ph * 4;
    o->pool_size = one * 2;
    int fd = memfd_create("facelight", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, o->pool_size) < 0) { perror("memfd"); exit(1); }
    o->pool_mem = mmap(NULL, o->pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, o->pool_size);
    for (int i = 0; i < 2; i++) {
        o->buf[i].data = (uint32_t *)((char *)o->pool_mem + one * i);
        o->buf[i].wl = wl_shm_pool_create_buffer(pool, one * i, o->pw, o->ph, o->pw * 4, WL_SHM_FORMAT_ARGB8888);
        o->buf[i].alpha = -1;
        wl_buffer_add_listener(o->buf[i].wl, &buffer_listener, &o->buf[i]);
    }
    wl_shm_pool_destroy(pool);
    close(fd);
    o->base = malloc(one);
    o->keep = malloc(sizeof(uint16_t) * HOLE_SIZE * HOLE_SIZE * o->scale * o->scale);
    render_base(o);
    render_keep(o);
    memset(o->shown_box, 0, sizeof o->shown_box);
    wl_surface_set_buffer_scale(o->surface, o->scale);
    render(o);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *l) {
    (void)l;
    free_buffers(data);
}
static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure, .closed = layer_closed};

static void create_surface(struct output *o) {
    o->surface = wl_compositor_create_surface(compositor);
    struct wl_region *empty = wl_compositor_create_region(compositor);
    wl_surface_set_input_region(o->surface, empty); // clicks pass through
    wl_region_destroy(empty);
    o->layer = zwlr_layer_shell_v1_get_layer_surface(layer_shell, o->surface, o->wl,
                                                    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "facelight");
    zwlr_layer_surface_v1_set_anchor(o->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                   ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(o->layer, 0);
    zwlr_layer_surface_v1_add_listener(o->layer, &layer_listener, o);
    o->w = o->h = 0;
    o->has_mouse = false;
    wl_surface_commit(o->surface);
}

static void destroy_surface(struct output *o) {
    free_buffers(o);
    if (o->layer) zwlr_layer_surface_v1_destroy(o->layer);
    if (o->surface) wl_surface_destroy(o->surface);
    o->layer = NULL;
    o->surface = NULL;
}

static void start_fade(double to) {
    fade_from = alpha_now / 256.0;
    fade_to = to;
    fade_start = now_ms();
}

static void update_state(void) {
    // A manual "off" lasts until the camera closes, so the next call lights up again.
    if (opt.mode == FORCE_OFF && !camera_on) opt.mode = AUTO;
    bool want = opt.mode == FORCE_ON || (opt.mode == AUTO && camera_on);
    if (want != want_shown) {
        want_shown = want;
        write_file("state", want ? "on\n" : "off\n");
    }
    if (want && !visible) {
        visible = true;
        alpha_now = 0;
        struct output *o;
        wl_list_for_each(o, &outputs, link) create_surface(o);
        start_fade(1);
        arm(tick_fd, TICK_MS, TICK_MS);
    } else if (want && visible && fade_to == 0) {
        start_fade(1); // camera came back during the fade-out
    } else if (!want && visible && fade_to != 0) {
        start_fade(0);
    }
}

static void tick(void) {
    double t = (now_ms() - fade_start) / FADE_MS;
    int a = (int)((fade_from + (fade_to - fade_from) * fmin(t, 1)) * 256 + 0.5);
    bool fade_changed = a != alpha_now;
    alpha_now = a;
    if (fade_to == 0 && t >= 1) { // fade-out done
        struct output *o;
        wl_list_for_each(o, &outputs, link) destroy_surface(o);
        visible = false;
        arm(tick_fd, 0, 0);
        return;
    }
    int cx = 0, cy = 0;
    bool have = opt.cursor_hole && hypr_cursor(&cx, &cy);
    if (have && getenv("FACELIGHT_BENCH")) { // fake cursor along the bottom edge, to measure CPU
        double s = now_ms() / 1000;
        cx = 960 + 800 * sin(s);
        cy = 1000 + 40 * sin(3 * s);
    }
    struct output *o;
    wl_list_for_each(o, &outputs, link) {
        if (!o->base) continue;
        int mx = (cx - o->x) * o->scale, my = (cy - o->y) * o->scale, pad = o->hole_px / 2;
        bool in = have && mx >= -pad && mx <= o->pw + pad && my >= -pad && my <= o->ph + pad;
        if (in != o->has_mouse || (in && (mx != o->mx || my != o->my))) {
            o->has_mouse = in;
            o->mx = mx;
            o->my = my;
            o->dirty = true;
        }
        if (o->dirty || fade_changed) render(o);
    }
}

// --- wl_output / registry -------------------------------------------------------

static void out_geometry(void *d, struct wl_output *w, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
                         const char *make, const char *model, int32_t tr) {
    (void)w; (void)pw; (void)ph; (void)sp; (void)make; (void)model; (void)tr;
    struct output *o = d;
    o->x = x;
    o->y = y;
}
static void out_mode(void *d, struct wl_output *w, uint32_t f, int32_t wd, int32_t h, int32_t r) {
    (void)d; (void)w; (void)f; (void)wd; (void)h; (void)r;
}
static void out_done(void *d, struct wl_output *w) { (void)d; (void)w; }
static void out_scale(void *d, struct wl_output *w, int32_t s) { (void)w; ((struct output *)d)->scale = s; }
static const struct wl_output_listener output_listener = {
    .geometry = out_geometry, .mode = out_mode, .done = out_done, .scale = out_scale};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, ver < 4 ? ver : 4);
    } else if (!strcmp(iface, wl_output_interface.name)) {
        struct output *o = calloc(1, sizeof *o);
        o->name = name;
        o->scale = 1;
        o->wl = wl_registry_bind(r, name, &wl_output_interface, ver < 3 ? ver : 3);
        wl_output_add_listener(o->wl, &output_listener, o);
        wl_list_insert(&outputs, &o->link);
        if (visible) create_surface(o);
    }
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {
    (void)d; (void)r;
    struct output *o, *tmp;
    wl_list_for_each_safe(o, tmp, &outputs, link) {
        if (o->name != name) continue;
        destroy_surface(o);
        wl_output_destroy(o->wl);
        wl_list_remove(&o->link);
        free(o);
    }
}
static const struct wl_registry_listener registry_listener = {.global = reg_global, .global_remove = reg_remove};

// --- client commands --------------------------------------------------------------

static void print_status(void) {
    char path[300], state[16] = "stopped";
    if (daemon_pid()) {
        runtime_path(path, sizeof path, "state");
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%15s", state) != 1) strcpy(state, "off");
            fclose(f);
        }
    }
    printf("{\"alt\": \"%s\", \"class\": \"%s\"}\n", state, state);
    fflush(stdout);
}

static int cmd_status(bool follow) {
    print_status();
    if (!follow) return 0;
    // Watch the runtime dir: state changes, and the daemon starting or stopping.
    mkdir(run_dir, 0700);
    int fd = inotify_init1(IN_CLOEXEC);
    inotify_add_watch(fd, run_dir, IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE);
    char buf[4096];
    while (read(fd, buf, sizeof buf) > 0) print_status();
    return 0;
}

static int cmd_signal(int sig) {
    pid_t pid = daemon_pid();
    if (!pid) {
        fprintf(stderr, "facelight: daemon is not running (start it with `facelight` or the systemd unit)\n");
        return 1;
    }
    return kill(pid, sig) == 0 ? 0 : 1;
}

// --- main -----------------------------------------------------------------------

static void usage(FILE *out, const char *argv0) {
    fprintf(out,
            "usage: %s [--on] [--width 0..1] [--color fff4e5] [--brightness 0..1] [--no-cursor-hole]\n"
            "       %s toggle | auto | status [--follow]\n",
            argv0, argv0);
}

static void parse_args(int argc, char **argv) {
    static struct option longs[] = {
        {"on", no_argument, 0, 'o'},           {"width", required_argument, 0, 'w'},
        {"color", required_argument, 0, 'c'},  {"brightness", required_argument, 0, 'b'},
        {"no-cursor-hole", no_argument, 0, 'n'}, {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}};
    int c;
    unsigned rgb;
    while ((c = getopt_long(argc, argv, "", longs, NULL)) != -1) {
        switch (c) {
        case 'o': opt.mode = FORCE_ON; break;
        case 'w': opt.width = fmin(fmax(atof(optarg), 0), 1); break;
        case 'b': opt.brightness = fmin(fmax(atof(optarg), 0), 1); break;
        case 'n': opt.cursor_hole = false; break;
        case 'c':
            if (sscanf(optarg[0] == '#' ? optarg + 1 : optarg, "%6x", &rgb) == 1) {
                opt.r = (rgb >> 16 & 0xff) / 255.0;
                opt.g = (rgb >> 8 & 0xff) / 255.0;
                opt.b = (rgb & 0xff) / 255.0;
            }
            break;
        default:
            usage(c == 'h' ? stdout : stderr, argv[0]);
            exit(c == 'h' ? 0 : 2);
        }
    }
}

int main(int argc, char **argv) {
    const char *rt = getenv("XDG_RUNTIME_DIR");
    if (!rt) { fprintf(stderr, "facelight: XDG_RUNTIME_DIR is not set\n"); return 1; }
    snprintf(run_dir, sizeof run_dir, "%s/facelight", rt);

    if (argc > 1 && argv[1][0] != '-') {
        if (!strcmp(argv[1], "toggle")) return cmd_signal(SIGUSR1);
        if (!strcmp(argv[1], "auto")) return cmd_signal(SIGUSR2);
        if (!strcmp(argv[1], "status")) return cmd_status(argc > 2 && !strcmp(argv[2], "--follow"));
        usage(stderr, argv[0]);
        return 2;
    }
    parse_args(argc, argv);
    if (daemon_pid()) { fprintf(stderr, "facelight: already running\n"); return 1; }
    mkdir(run_dir, 0700);
    char pid_text[32];
    snprintf(pid_text, sizeof pid_text, "%d\n", getpid());
    write_file("pid", pid_text);

    const char *sig = getenv("HYPRLAND_INSTANCE_SIGNATURE");
    if (sig) snprintf(hypr_sock, sizeof hypr_sock, "%s/hypr/%s/.socket.sock", rt, sig);

    display = wl_display_connect(NULL);
    if (!display) { fprintf(stderr, "facelight: no Wayland display\n"); return 1; }
    wl_list_init(&outputs);
    struct wl_registry *reg = wl_display_get_registry(display);
    wl_registry_add_listener(reg, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !layer_shell) {
        fprintf(stderr, "facelight: the compositor does not support wlr-layer-shell\n");
        return 1;
    }

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    sigaddset(&mask, SIGUSR2);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    sig_fd = signalfd(-1, &mask, SFD_CLOEXEC);
    ino_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    tick_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    settle_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    fallback_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    add_watches();
    arm(fallback_fd, FALLBACK_S * 1000, FALLBACK_S * 1000);
    camera_on = camera_in_use();
    want_shown = !(opt.mode == FORCE_ON || camera_on); // force the first state write
    update_state();

    struct pollfd fds[] = {
        {wl_display_get_fd(display), POLLIN, 0}, {sig_fd, POLLIN, 0},      {ino_fd, POLLIN, 0},
        {tick_fd, POLLIN, 0},                    {settle_fd, POLLIN, 0}, {fallback_fd, POLLIN, 0},
    };
    char drain[4096];
    uint64_t exp;
    bool quitting = false;
    for (;;) {
        while (wl_display_prepare_read(display) != 0) wl_display_dispatch_pending(display);
        wl_display_flush(display);
        if (poll(fds, 6, -1) < 0) {
            wl_display_cancel_read(display);
            if (errno == EINTR) continue;
            break;
        }
        if (fds[0].revents & POLLIN) wl_display_read_events(display);
        else wl_display_cancel_read(display);
        if (fds[0].revents & (POLLERR | POLLHUP)) break;
        wl_display_dispatch_pending(display);

        if (fds[1].revents & POLLIN) {
            struct signalfd_siginfo si;
            if (read(sig_fd, &si, sizeof si) == sizeof si) {
                if (si.ssi_signo == SIGUSR1) opt.mode = want_shown ? FORCE_OFF : FORCE_ON;
                else if (si.ssi_signo == SIGUSR2) opt.mode = AUTO;
                else quitting = true, opt.mode = FORCE_OFF, camera_on = true;
                update_state();
                if (quitting && !visible) break;
            }
        }
        if (fds[2].revents & POLLIN) {
            while (read(ino_fd, drain, sizeof drain) > 0) {}
            arm(settle_fd, SETTLE_MS, 0);
        }
        if (fds[4].revents & POLLIN) {
            if (read(settle_fd, &exp, sizeof exp) > 0) {}
            if (!quitting) camera_on = camera_in_use();
            update_state();
        }
        if (fds[5].revents & POLLIN) {
            if (read(fallback_fd, &exp, sizeof exp) > 0) {}
            add_watches(); // cameras plugged in after startup
            if (!quitting) camera_on = camera_in_use();
            update_state();
        }
        if (fds[3].revents & POLLIN) {
            if (read(tick_fd, &exp, sizeof exp) > 0) {}
            if (visible) tick();
            if (quitting && !visible) break;
        }
    }
    char path[300];
    runtime_path(path, sizeof path, "state");
    unlink(path);
    runtime_path(path, sizeof path, "pid");
    unlink(path);
    wl_display_disconnect(display);
    return 0;
}
