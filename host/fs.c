// File syscalls of the Emscripten libc, served from the host file system. Guest paths under /game/ map to the read-only
// data folder; every other absolute path maps into the writable user folder.
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#include "host.h"

// Emscripten's errno numbers are the WASI ones.
enum { G_EACCES = 2, G_EAFNOSUPPORT = 5, G_EAGAIN = 6, G_EBADF = 8, G_EEXIST = 20, G_EINVAL = 28, G_EIO = 29, G_EISDIR = 31,
       G_EMFILE = 33, G_ENAMETOOLONG = 37, G_ENOENT = 44, G_ENOMEM = 48, G_ENOSPC = 51, G_ENOSYS = 52, G_ENOTDIR = 54,
       G_ENOTEMPTY = 55, G_ENOTTY = 59, G_EPERM = 63, G_EROFS = 69, G_ESPIPE = 70, G_EXDEV = 75 };

static u32 gerr(int e) {
  switch (e) {
    case EACCES: return G_EACCES; case EAGAIN: return G_EAGAIN; case EBADF: return G_EBADF; case EEXIST: return G_EEXIST;
    case EINVAL: return G_EINVAL; case EISDIR: return G_EISDIR; case EMFILE: return G_EMFILE; case ENAMETOOLONG: return G_ENAMETOOLONG;
    case ENOENT: return G_ENOENT; case ENOMEM: return G_ENOMEM; case ENOSPC: return G_ENOSPC; case ENOTDIR: return G_ENOTDIR;
    case ENOTEMPTY: return G_ENOTEMPTY; case EPERM: return G_EPERM; case EROFS: return G_EROFS; case ESPIPE: return G_ESPIPE;
    case EXDEV: return G_EXDEV;
  }
  return G_EIO;
}
#define NEG(e) ((u32)-(int)(e))

// ---- guest file descriptors -----------------------------------------------------------------------------------------

#define MAX_FD 1024
typedef struct {
  int host;      // -1 = free
  DIR* dir;      // opened lazily by getdents
  u64 dir_index;
} gfd;
static gfd fds[MAX_FD];
static pthread_mutex_t fd_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t fd_once = PTHREAD_ONCE_INIT;

static void fd_init(void) {
  for (int i = 0; i < MAX_FD; i++) fds[i].host = -1;
  fds[0].host = 0; fds[1].host = 1; fds[2].host = 2;
}

static int fd_add(int host) {
  pthread_once(&fd_once, fd_init);
  pthread_mutex_lock(&fd_mu);
  int g = -1;
  for (int i = 3; i < MAX_FD; i++)
    if (fds[i].host < 0) { fds[i] = (gfd){ .host = host }; g = i; break; }
  pthread_mutex_unlock(&fd_mu);
  return g;
}

static gfd* fd_get(u32 g) {
  pthread_once(&fd_once, fd_init);
  return g < MAX_FD && fds[g].host >= 0 ? &fds[g] : NULL;
}

// ---- paths ----------------------------------------------------------------------------------------------------------

// Resolves a guest path to a host path. Returns false for a path that climbs out of its root.
static bool host_path(u32 dirfd, const char* guest, char* out, size_t cap) {
  char norm[1024];
  size_t n = 0;
  norm[n++] = '/';
  for (const char* p = guest; *p;) {
    while (*p == '/') p++;
    const char* e = p;
    while (*e && *e != '/') e++;
    size_t len = (size_t)(e - p);
    if (len == 2 && p[0] == '.' && p[1] == '.') {
      if (n <= 1) return false;
      n--;
      while (n > 1 && norm[n - 1] != '/') n--;
    } else if (len && !(len == 1 && p[0] == '.')) {
      if (n + len + 2 > sizeof norm) return false;
      memcpy(norm + n, p, len);
      n += len;
      norm[n++] = '/';
    }
    p = e;
  }
  if (n > 1) n--;
  norm[n] = 0;
  if (!strncmp(norm, "/dev/", 5)) return snprintf(out, cap, "%s", norm) < (int)cap;
  if (!strncmp(norm, "/game/", 6) || !strcmp(norm, "/game")) return snprintf(out, cap, "%s%s", g_data_dir, norm + 5) < (int)cap;
  return snprintf(out, cap, "%s%s", g_user_dir, norm) < (int)cap;
}

static void put_stat(u64 buf, const struct stat* st) {
  memset(gptr(buf), 0, 104);
  wr32(buf, (u32)st->st_dev); wr32(buf + 4, st->st_mode); wr64(buf + 8, st->st_nlink);
  wr32(buf + 16, st->st_uid); wr32(buf + 20, st->st_gid); wr32(buf + 24, (u32)st->st_rdev);
  wr64(buf + 32, (u64)st->st_size); wr32(buf + 40, 4096); wr32(buf + 44, (u32)st->st_blocks);
  wr64(buf + 48, (u64)st->st_atimespec.tv_sec); wr64(buf + 56, (u64)st->st_atimespec.tv_nsec);
  wr64(buf + 64, (u64)st->st_mtimespec.tv_sec); wr64(buf + 72, (u64)st->st_mtimespec.tv_nsec);
  wr64(buf + 80, (u64)st->st_ctimespec.tv_sec); wr64(buf + 88, (u64)st->st_ctimespec.tv_nsec);
  wr64(buf + 96, st->st_ino);
}

static u32 stat_path(u32 dirfd, u64 path, u64 buf, bool follow) {
  char hp[2048];
  struct stat st;
  if (!host_path(dirfd, gptr(path), hp, sizeof hp)) return NEG(G_ENOENT);
  int r = follow ? stat(hp, &st) : lstat(hp, &st);
  if (g_verbose > 1) host_log("stat %s -> %s", (const char*)gptr(path), r ? strerror(errno) : "ok");
  if (r) return NEG(gerr(errno));
  put_stat(buf, &st);
  return 0;
}

// ---- syscalls -------------------------------------------------------------------------------------------------------

u32 w2c_env_0x5F_syscall_openat(struct w2c_env* env, u32 dirfd, u64 path, u32 flags, u64 varargs) {
  char hp[2048];
  if (!host_path(dirfd, gptr(path), hp, sizeof hp)) return NEG(G_ENOENT);
  int hf = (int)(flags & 3);
  if (flags & 0100) hf |= O_CREAT;
  if (flags & 0200) hf |= O_EXCL;
  if (flags & 01000) hf |= O_TRUNC;
  if (flags & 02000) hf |= O_APPEND;
  if (flags & 0200000) hf |= O_DIRECTORY;
  int h = open(hp, hf | O_CLOEXEC, (flags & 0100) ? (mode_t)rd32(varargs) : 0);
  if (g_verbose) host_log("open %s (flags %o) -> %s", (const char*)gptr(path), flags, h < 0 ? strerror(errno) : "ok");
  if (h < 0) return NEG(gerr(errno));
  int g = fd_add(h);
  if (g < 0) { close(h); return NEG(G_EMFILE); }
  return (u32)g;
}

u32 w2c_wasi__snapshot__preview1_fd_close(struct w2c_wasi__snapshot__preview1* w, u32 fd) {
  gfd* f = fd_get(fd);
  if (!f) return G_EBADF;
  if (fd > 2) {
    if (f->dir) closedir(f->dir); else close(f->host);
    pthread_mutex_lock(&fd_mu);
    f->host = -1; f->dir = NULL;
    pthread_mutex_unlock(&fd_mu);
  }
  return 0;
}

// Runs one readv/writev-style call over the guest iovec array (16 bytes per entry: pointer, length).
static u32 do_iov(u32 fd, u64 iov, u64 count, int64_t offset, u64 out, bool writing) {
  gfd* f = fd_get(fd);
  if (!f) return G_EBADF;
  u64 total = 0;
  for (u64 i = 0; i < count; i++) {
    void* p = gptr(rd64(iov + 16 * i));
    size_t len = rd64(iov + 16 * i + 8);
    if (!len) continue;
    ssize_t r = writing ? (offset < 0 ? write(f->host, p, len) : pwrite(f->host, p, len, offset + (off_t)total))
                        : (offset < 0 ? read(f->host, p, len) : pread(f->host, p, len, offset + (off_t)total));
    if (r < 0) { if (total) break; return gerr(errno); }
    total += (u64)r;
    if ((size_t)r < len) break;
  }
  wr64(out, total);
  return 0;
}

u32 w2c_wasi__snapshot__preview1_fd_write(struct w2c_wasi__snapshot__preview1* w, u32 fd, u64 iov, u64 n, u64 out) { return do_iov(fd, iov, n, -1, out, true); }
u32 w2c_wasi__snapshot__preview1_fd_read(struct w2c_wasi__snapshot__preview1* w, u32 fd, u64 iov, u64 n, u64 out) { return do_iov(fd, iov, n, -1, out, false); }
u32 w2c_wasi__snapshot__preview1_fd_pwrite(struct w2c_wasi__snapshot__preview1* w, u32 fd, u64 iov, u64 n, u64 off, u64 out) { return do_iov(fd, iov, n, (int64_t)off, out, true); }
u32 w2c_wasi__snapshot__preview1_fd_pread(struct w2c_wasi__snapshot__preview1* w, u32 fd, u64 iov, u64 n, u64 off, u64 out) { return do_iov(fd, iov, n, (int64_t)off, out, false); }

u32 w2c_wasi__snapshot__preview1_fd_seek(struct w2c_wasi__snapshot__preview1* w, u32 fd, u64 offset, u32 whence, u64 out) {
  gfd* f = fd_get(fd);
  if (!f) return G_EBADF;
  off_t r = lseek(f->host, (off_t)(int64_t)offset, whence == 0 ? SEEK_SET : whence == 1 ? SEEK_CUR : SEEK_END);
  if (r < 0) return gerr(errno);
  wr64(out, (u64)r);
  return 0;
}

u32 w2c_env_0x5F_syscall_stat64(struct w2c_env* env, u64 path, u64 buf) { return stat_path(0, path, buf, true); }
u32 w2c_env_0x5F_syscall_lstat64(struct w2c_env* env, u64 path, u64 buf) { return stat_path(0, path, buf, false); }
u32 w2c_env_0x5F_syscall_newfstatat(struct w2c_env* env, u32 dirfd, u64 path, u64 buf, u32 flags) { return stat_path(dirfd, path, buf, !(flags & 0x100)); }

u32 w2c_env_0x5F_syscall_fstat64(struct w2c_env* env, u32 fd, u64 buf) {
  gfd* f = fd_get(fd);
  struct stat st;
  if (!f) return NEG(G_EBADF);
  if (fstat(f->host, &st)) return NEG(gerr(errno));
  put_stat(buf, &st);
  return 0;
}

u32 w2c_env_0x5F_syscall_ftruncate64(struct w2c_env* env, u32 fd, u64 length) {
  gfd* f = fd_get(fd);
  if (!f) return NEG(G_EBADF);
  return ftruncate(f->host, (off_t)length) ? NEG(gerr(errno)) : 0;
}

u32 w2c_env_0x5F_syscall_getcwd(struct w2c_env* env, u64 buf, u64 size) {
  if (size < 2) return NEG(68);  // ERANGE
  memcpy(gptr(buf), "/", 2);
  return 2;
}

u32 w2c_env_0x5F_syscall_mkdirat(struct w2c_env* env, u32 dirfd, u64 path, u32 mode) {
  char hp[2048];
  if (!host_path(dirfd, gptr(path), hp, sizeof hp)) return NEG(G_ENOENT);
  return mkdir(hp, (mode_t)mode | 0700) ? NEG(gerr(errno)) : 0;
}

u32 w2c_env_0x5F_syscall_unlinkat(struct w2c_env* env, u32 dirfd, u64 path, u32 flags) {
  char hp[2048];
  if (!host_path(dirfd, gptr(path), hp, sizeof hp)) return NEG(G_ENOENT);
  return ((flags & 0x200) ? rmdir(hp) : unlink(hp)) ? NEG(gerr(errno)) : 0;
}

u32 w2c_env_0x5F_syscall_rmdir(struct w2c_env* env, u64 path) {
  char hp[2048];
  if (!host_path(0, gptr(path), hp, sizeof hp)) return NEG(G_ENOENT);
  return rmdir(hp) ? NEG(gerr(errno)) : 0;
}

u32 w2c_env_0x5F_syscall_renameat(struct w2c_env* env, u32 olddirfd, u64 oldpath, u32 newdirfd, u64 newpath) {
  char a[2048], b[2048];
  if (!host_path(olddirfd, gptr(oldpath), a, sizeof a) || !host_path(newdirfd, gptr(newpath), b, sizeof b)) return NEG(G_ENOENT);
  return rename(a, b) ? NEG(gerr(errno)) : 0;
}

// struct dirent of the guest: ino (8), off (8), reclen (2), type (1), name (256) = 280 bytes
u32 w2c_env_0x5F_syscall_getdents64(struct w2c_env* env, u32 fd, u64 dirp, u64 count) {
  gfd* f = fd_get(fd);
  if (!f) return NEG(G_EBADF);
  if (!f->dir && !(f->dir = fdopendir(f->host))) return NEG(gerr(errno));
  u64 pos = 0;
  while (pos + 280 <= count) {
    struct dirent* d = readdir(f->dir);
    if (!d) break;
    u64 e = dirp + pos;
    memset(gptr(e), 0, 280);
    wr64(e, d->d_ino);
    wr64(e + 8, (++f->dir_index) * 280);
    uint16_t reclen = 280;
    memcpy(gptr(e + 16), &reclen, 2);
    *(uint8_t*)gptr(e + 18) = d->d_type;
    snprintf(gptr(e + 19), 256, "%s", d->d_name);
    pos += 280;
  }
  return (u32)pos;
}

u32 w2c_env_0x5F_syscall_statfs64(struct w2c_env* env, u64 path, u64 size, u64 buf) {
  memset(gptr(buf), 0, size);
  wr32(buf + 8, 4096); wr32(buf + 72, 4096);
  wr64(buf + 16, 1u << 24); wr64(buf + 24, 1u << 23); wr64(buf + 32, 1u << 23);
  return 0;
}

u32 w2c_env_0x5F_syscall_fcntl64(struct w2c_env* env, u32 fd, u32 cmd, u64 varargs) {
  if (!fd_get(fd)) return NEG(G_EBADF);
  if (cmd == 0 || cmd == 1030) return NEG(G_EINVAL);  // F_DUPFD, F_DUPFD_CLOEXEC
  return 0;
}

u32 w2c_env_0x5F_syscall_ioctl(struct w2c_env* env, u32 fd, u32 op, u64 varargs) { return NEG(G_ENOTTY); }

u32 w2c_wasi__snapshot__preview1_environ_sizes_get(struct w2c_wasi__snapshot__preview1* w, u64 count, u64 size) {
  wr64(count, 0); wr64(size, 0);
  return 0;
}
u32 w2c_wasi__snapshot__preview1_environ_get(struct w2c_wasi__snapshot__preview1* w, u64 env, u64 buf) { return 0; }

// ---- sockets: the engine runs with -nonetwork ---------------------------------------------------------------------------

#define NO_NET(name) host_once(name, "unhandled: socket call %s", name); return NEG(G_EAFNOSUPPORT)
u32 w2c_env_0x5F_syscall_socket(struct w2c_env* env, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) { NO_NET("socket"); }
u32 w2c_env_0x5F_syscall_connect(struct w2c_env* env, u32 a, u64 b, u32 c, u32 d, u32 e, u32 f) { NO_NET("connect"); }
u32 w2c_env_0x5F_syscall_accept4(struct w2c_env* env, u32 a, u64 b, u64 c, u32 d, u32 e, u32 f) { NO_NET("accept4"); }
u32 w2c_env_0x5F_syscall_getsockname(struct w2c_env* env, u32 a, u64 b, u64 c, u32 d, u32 e, u32 f) { NO_NET("getsockname"); }
u32 w2c_env_0x5F_syscall_getsockopt(struct w2c_env* env, u32 a, u32 b, u32 c, u64 d, u64 e, u32 f) { NO_NET("getsockopt"); }
u32 w2c_env_0x5F_syscall_setsockopt(struct w2c_env* env, u32 a, u32 b, u32 c, u64 d, u32 e, u32 f) { NO_NET("setsockopt"); }
u32 w2c_env_0x5F_syscall_recvfrom(struct w2c_env* env, u32 a, u64 b, u64 c, u32 d, u64 e, u64 f) { NO_NET("recvfrom"); }
u32 w2c_env_0x5F_syscall_sendto(struct w2c_env* env, u32 a, u64 b, u64 c, u32 d, u64 e, u32 f) { NO_NET("sendto"); }
u32 w2c_env_0x5F_syscall_shutdown(struct w2c_env* env, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) { NO_NET("shutdown"); }
u32 w2c_env_0x5F_syscall_poll(struct w2c_env* env, u64 fds_ptr, u32 nfds, u32 timeout) { host_once("poll", "unhandled: poll"); return 0; }
