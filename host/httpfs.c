// The engine's game-data reads (platform/file/httpfs_wasm.cpp on the engine side). In the browser io_worker.js answers
// them with HTTP range requests; here one thread answers them with pread() on the mirrored data folder.
//
// Protocol, all in the shared heap at `table` (int32 words; see io_worker.js):
//   header: [0] doorbell, [1] slot count, [2] hint ring offset in words, [3] its entries, [4] hints claimed, [5] hints consumed
//   slot i (16 words at 16 + 16*i): [0] state 0 idle / 1 requested / 3 taken / 2 done, [1] file id, [2..3] offset,
//                                   [4..5] destination address, [6] length, [7] result (bytes or -1)
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host.h"

static char* manifest;
static size_t manifest_len;
static pthread_once_t manifest_once = PTHREAD_ONCE_INIT;

typedef struct {
  char* path;
  int fd;  // 0 = not opened yet, -1 = failed
} data_file;
static data_file* files;
static size_t nfiles;

// The manifest's "files" array is [["path",size,mtime],...] in file-id order.
static void parse_files(void) {
  const char* p = strstr(manifest, "\"files\":[");
  if (!p) return;
  p += 9;
  size_t cap = 8192;
  files = malloc(cap * sizeof *files);
  while ((p = strstr(p, "[\""))) {
    p += 2;
    const char* e = p;
    while (*e && *e != '"') e += (*e == '\\' && e[1]) ? 2 : 1;
    if (nfiles == cap) files = realloc(files, (cap *= 2) * sizeof *files);
    char* path = malloc((size_t)(e - p) + 1);
    size_t n = 0;
    for (const char* c = p; c < e; c++) {
      if (*c == '\\' && c + 1 < e) c++;
      path[n++] = *c;
    }
    path[n] = 0;
    files[nfiles++] = (data_file){ .path = path };
    p = e;
  }
}

static void load_manifest(void) {
  char path[2048];
  snprintf(path, sizeof path, "%s/manifest.json", g_data_dir);
  FILE* f = fopen(path, "rb");
  if (!f) { host_log("cannot open %s", path); return; }
  fseek(f, 0, SEEK_END);
  manifest_len = (size_t)ftell(f);
  rewind(f);
  manifest = malloc(manifest_len + 1);
  if (fread(manifest, 1, manifest_len, f) != manifest_len) manifest_len = 0;
  manifest[manifest_len] = 0;
  fclose(f);
  parse_files();
  host_log("data manifest: %zu files", nfiles);
}

// Returns the manifest's length; copies it (NUL-terminated) when the buffer is large enough.
u32 w2c_env_wasm_httpfs_manifest_js(struct w2c_env* env, u64 buf, u32 cap) {
  pthread_once(&manifest_once, load_manifest);
  if (!manifest_len) return (u32)-1;
  if (cap > manifest_len) memcpy(gptr(buf), manifest, manifest_len + 1);
  return (u32)manifest_len;
}

static int32_t serve(u64 w) {
  u32 id = (u32)atomic_load(aword(w + 1));
  u64 off = (u32)atomic_load(aword(w + 2)) | (u64)(u32)atomic_load(aword(w + 3)) << 32;
  u64 dst = (u32)atomic_load(aword(w + 4)) | (u64)(u32)atomic_load(aword(w + 5)) << 32;
  int32_t len = atomic_load(aword(w + 6));
  if (id >= nfiles || len < 0) return -1;
  data_file* f = &files[id];
  if (!f->fd) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", g_data_dir, f->path);
    f->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (f->fd < 0 && (errno == EMFILE || errno == ENFILE)) {
      // out of descriptors (an iPhone app gets few): close every data file, they reopen when next read. Only this thread uses them.
      for (size_t i = 0; i < nfiles; i++) if (files[i].fd > 0) { close(files[i].fd); files[i].fd = 0; }
      f->fd = open(path, O_RDONLY | O_CLOEXEC);
    }
    if (f->fd < 0) host_log("cannot open data file %s: %s", path, strerror(errno));
    else if (g_verbose) host_log("data file %s", f->path);
  }
  if (f->fd < 0) return -1;
  size_t got = 0;
  while (got < (size_t)len) {
    ssize_t r = pread(f->fd, gptr(dst + got), (size_t)len - got, (off_t)(off + got));
    if (r < 0) return -1;
    if (!r) break;
    got += (size_t)r;
  }
  return (int32_t)got;
}

static void* io_main(void* p) {
  u64 bell = (u64)(uintptr_t)p / 4;
  for (;;) {
    int32_t seen = atomic_load(aword(bell));
    int32_t n = atomic_load(aword(bell + 1));
    for (int32_t slot = 0; slot < n; slot++) {
      u64 w = bell + 16 + 16 * (u64)slot;
      int32_t requested = 1;
      if (!atomic_compare_exchange_strong(aword(w), &requested, 3)) continue;
      atomic_store(aword(w + 7), serve(w));
      atomic_store(aword(w), 2);
      w2c_host_atomic_notify(&g_mem, w * 4, 1);
    }
    // Read-ahead hints only matter over a network: mark them consumed.
    u32 ring = (u32)atomic_load(aword(bell + 2)), cap = (u32)atomic_load(aword(bell + 3));
    if (ring && cap) {
      u32 head = (u32)atomic_load(aword(bell + 4)), tail = (u32)atomic_load(aword(bell + 5));
      if (head - tail > cap) tail = head - cap;
      for (; tail != head; tail++) {
        u64 e = bell + ring + 4 * (u64)(tail % cap);
        if (!atomic_load(aword(e + 3))) break;
        atomic_store(aword(e + 3), 0);
      }
      atomic_store(aword(bell + 5), (int32_t)tail);
    }
    w2c_host_atomic_wait32(&g_mem, bell * 4, (u32)seen, (u64)-1);
  }
  return NULL;
}

void w2c_env_wasm_httpfs_io_start_js(struct w2c_env* env, u64 table) {
  static atomic_flag started = ATOMIC_FLAG_INIT;
  if (atomic_flag_test_and_set(&started)) return;
  pthread_once(&manifest_once, load_manifest);
  host_log("data I/O table at %llx", (unsigned long long)table);
  pthread_t t;
  pthread_create(&t, NULL, io_main, (void*)(uintptr_t)table);
  pthread_detach(t);
}

// Savegame persistence through the page's IndexedDB: not needed, /userdata is already a real folder here.
u32 w2c_env_wasm_userdata_page_js(struct w2c_env* env) { return 0; }
void w2c_env_wasm_userdata_put_js(struct w2c_env* env, u64 path, u64 data, u64 size, f64 mtime_ms) {}
void w2c_env_wasm_userdata_delete_js(struct w2c_env* env, u64 path) {}
