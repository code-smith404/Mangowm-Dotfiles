/* mangowall - minimal swaybg replacement for mango (wlr-layer-shell background)
 * Renders a PNG "cover" style on every output. Build:
 *   cc mangowall.c wlr-layer-shell-unstable-v1-protocol.c -o mangowall \
 *      $(pkg-config --cflags --libs libpng wayland-client)
 */
#include <errno.h>
#include <fcntl.h>
#include <png.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static int img_w, img_h;
static uint8_t *img; /* RGBA */

static int load_png(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) { perror("fopen"); return -1; }
  uint8_t sig[8];
  if (fread(sig, 1, 8, f) != 8 || png_sig_cmp(sig, 0, 8)) {
    fprintf(stderr, "not a png: %s\n", path); fclose(f); return -1;
  }
  png_structp p = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
  png_infop i = png_create_info_struct(p);
  if (!p || !i || setjmp(png_jmpbuf(p))) { png_destroy_read_struct(&p, &i, NULL); fclose(f); return -1; }
  png_init_io(p, f);
  png_set_sig_bytes(p, 8);
  png_read_info(p, i);
  img_w = png_get_image_width(p, i);
  img_h = png_get_image_height(p, i);
  png_set_expand(p);
  png_set_strip_16(p);
  /* No png_set_gamma here on purpose: the image is already sRGB-encoded and the
   * display is sRGB, so the raw file values are what should reach the screen.
   * png_set_gamma(p,2.2,2.2) is NOT a no-op -- libpng decodes and re-encodes with
   * gamma 2.2, applying the 1/2.2 curve twice and washing dark tones out
   * (measured: mean RGB 110,80,134 -> 208,196,221). */
  png_read_update_info(p, i);
  int bpp = (int)png_get_rowbytes(p, i) / img_w;
  uint8_t *rows = malloc((size_t)img_h * png_get_rowbytes(p, i));
  png_bytep *rp = malloc(sizeof(png_bytep) * img_h);
  for (int y = 0; y < img_h; y++) rp[y] = rows + (size_t)y * png_get_rowbytes(p, i);
  png_read_image(p, rp);
  png_read_end(p, NULL);
  /* convert to RGBA */
  img = malloc((size_t)img_w * img_h * 4);
  for (int y = 0; y < img_h; y++) {
    uint8_t *src = rp[y], *dst = img + (size_t)y * img_w * 4;
    for (int x = 0; x < img_w; x++) {
      if (bpp == 4) { dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=src[3]; }
      else if (bpp == 3) { dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=255; }
      else if (bpp == 2) { dst[0]=dst[1]=dst[2]=src[0]; dst[3]=src[1]; }
      else { dst[0]=dst[1]=dst[2]=src[0]; dst[3]=255; }
      src += bpp; dst += 4;
    }
  }
  free(rp); free(rows);
  png_destroy_read_struct(&p, &i, NULL);
  fclose(f);
  return 0;
}

struct output {
  struct wl_output *wl;
  struct zwlr_layer_surface_v1 *ls;
  struct wl_surface *surf;
  struct wl_buffer *buf;
  void *shm_data;
  uint32_t name;
  int w, h, configured;
  struct output *next;
};

static struct wl_compositor *comp;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *lshell;
static struct output *outputs;
static const char *wall_path;

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
    int32_t pw, int32_t ph, int32_t sub, const char *mk, const char *md,
    int32_t transform) { (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sub;(void)mk;(void)md;(void)transform; }
static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) { (void)d;(void)o;(void)flags;(void)w;(void)h;(void)refresh; }
static void output_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t s) { (void)d;(void)o;(void)s; }
static void output_name(void *d, struct wl_output *o, const char *n) { (void)d;(void)o;(void)n; }
static const struct wl_output_listener outl = {
  output_geometry, output_mode, output_done, output_scale, output_name
};

static void lscfg(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
    uint32_t w, uint32_t h);

static void render(struct output *o, uint32_t w, uint32_t h);

static void ls_closed(void *d, struct zwlr_layer_surface_v1 *ls) {
  (void)d;(void)ls;
}
static const struct zwlr_layer_surface_v1_listener lsl = { lscfg, ls_closed };

static int memfd_create_(const char *name) {
  return (int)syscall(SYS_memfd_create, name, 0);
}

static void buffer_release(void *d, struct wl_buffer *b) { (void)d;(void)b; }
static const struct wl_buffer_listener bufl = { buffer_release };

static void render(struct output *o, uint32_t w, uint32_t h) {
  size_t size = (size_t)w * h * 4;
  if (o->buf) { wl_buffer_destroy(o->buf); o->buf = NULL; }
  if (o->shm_data) { munmap(o->shm_data, (size_t)o->w * o->h * 4); o->shm_data = NULL; }
  int fd = memfd_create_("mangowall");
  if (fd < 0) return;
  if (ftruncate(fd, (off_t)size) < 0) { close(fd); return; }
  uint8_t *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (data == MAP_FAILED) { close(fd); return; }
  struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
  struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h,
      (int32_t)w * 4, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  wl_buffer_add_listener(buf, &bufl, NULL);

  /* cover fit */
  double sc = (double)w / img_w > (double)h / img_h ? (double)w / img_w : (double)h / img_h;
  uint32_t cw = (uint32_t)(img_w * sc), chh = (uint32_t)(img_h * sc);
  uint32_t ox = (cw - w) / 2, oy = (chh - h) / 2;
  for (uint32_t y = 0; y < h; y++) {
    uint32_t sy = (uint32_t)((y + oy) / sc);
    if (sy >= (uint32_t)img_h) sy = img_h - 1;
    const uint8_t *src = img + (size_t)sy * img_w * 4;
    uint8_t *dst = data + (size_t)y * w * 4;
    for (uint32_t x = 0; x < w; x++) {
      uint32_t sx = (uint32_t)((x + ox) / sc);
      if (sx >= (uint32_t)img_w) sx = img_w - 1;
      const uint8_t *s = src + (size_t)sx * 4;
      /* premultiplied ARGB in memory: B G R A */
      dst[0] = (uint8_t)((s[2] * s[3]) / 255);
      dst[1] = (uint8_t)((s[1] * s[3]) / 255);
      dst[2] = (uint8_t)((s[0] * s[3]) / 255);
      dst[3] = s[3];
      dst += 4;
    }
  }
  o->buf = buf;
  o->shm_data = data;
  o->w = (int)w; o->h = (int)h;
  wl_surface_attach(o->surf, buf, 0, 0);
  wl_surface_damage_buffer(o->surf, 0, 0, (int32_t)w, (int32_t)h);
  wl_surface_commit(o->surf);
}

static void lscfg(void *d, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
    uint32_t w, uint32_t h) {
  (void)d;
  zwlr_layer_surface_v1_ack_configure(ls, serial);
  struct output *o = d;
  o->configured = 1;
  render(o, w ? w : 1920, h ? h : 1080);
}

static void globals(void *d, struct wl_registry *r, uint32_t name,
    const char *iface, uint32_t ver) {
  (void)d;
  if (strcmp(iface, wl_compositor_interface.name) == 0) {
    comp = wl_registry_bind(r, name, &wl_compositor_interface, 4);
  } else if (strcmp(iface, wl_shm_interface.name) == 0) {
    shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
  } else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
    lshell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface,
        ver < 3 ? ver : 3);
  } else if (strcmp(iface, wl_output_interface.name) == 0) {
    struct wl_output *o = wl_registry_bind(r, name, &wl_output_interface, 3);
    struct output *out = calloc(1, sizeof(*out));
    out->wl = o; out->name = name; out->next = outputs; outputs = out;
    wl_output_add_listener(o, &outl, NULL);
    wl_output_set_user_data(o, out);
  }
}
static void globals_rm(void *d, struct wl_registry *r, uint32_t name) {
  (void)d;(void)r;
  for (struct output **pp = &outputs; *pp; pp = &(*pp)->next) {
    if ((*pp)->name == name) {
      struct output *o = *pp;
      *pp = o->next;
      if (o->buf) wl_buffer_destroy(o->buf);
      if (o->ls) zwlr_layer_surface_v1_destroy(o->ls);
      if (o->surf) wl_surface_destroy(o->surf);
      free(o);
      break;
    }
  }
}
static const struct wl_registry_listener regl = { globals, globals_rm };

int main(int argc, char **argv) {
  wall_path = argc > 1 ? argv[1] : "/home/code_smith/.config/mango/wallpaper.png";
  if (load_png(wall_path) < 0) return 1;
  struct wl_display *disp = wl_display_connect(NULL);
  if (!disp) { fprintf(stderr, "mangowall: no wayland display\n"); return 1; }
  struct wl_registry *reg = wl_display_get_registry(disp);
  wl_registry_add_listener(reg, &regl, NULL);
  wl_display_roundtrip(disp);
  wl_display_roundtrip(disp);
  if (!comp || !shm || !lshell) {
    fprintf(stderr, "mangowall: missing globals\n"); return 1;
  }
  for (struct output *o = outputs; o; o = o->next) {
    o->surf = wl_compositor_create_surface(comp);
    o->ls = zwlr_layer_shell_v1_get_layer_surface(lshell, o->surf, o->wl,
        ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND, "mangowall");
    zwlr_layer_surface_v1_add_listener(o->ls, &lsl, o);
    zwlr_layer_surface_v1_set_anchor(o->ls,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(o->ls, -1);
    zwlr_layer_surface_v1_set_size(o->ls, 0, 0);
    wl_surface_commit(o->surf);
  }
  wl_display_roundtrip(disp);
  /* re-render on output changes */
  while (wl_display_dispatch(disp) >= 0) {
    int dirty = 0;
    for (struct output *o = outputs; o; o = o->next)
      if (o->configured && !o->buf) { dirty = 1; }
    (void)dirty;
  }
  return 0;
}
