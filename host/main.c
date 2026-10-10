// Entry point, linear memory, threads, futexes and the Emscripten runtime imports.
#include <errno.h>
#include <execinfo.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <TargetConditionals.h>
#include <glob.h>
#include <dlfcn.h>
#include <mach/mach.h>
#ifdef GTA_HEAP_BASE
#include <mach/mach_vm.h>
#endif

#include "gpu_host.h"
#include "host.h"

#define PAGE 65536ull
// wasm locals live on the native stack. An iPhone app has only a few GB of address space for ~35 engine threads.
#if TARGET_OS_IPHONE
#define MAIN_STACK (32u << 20)
#define THREAD_STACK (8u << 20)
#define DEFAULT_HEAP_MAX (4608ull << 20)
#else
#define MAIN_STACK (64u << 20)
#define THREAD_STACK (64u << 20)
#define DEFAULT_HEAP_MAX (16ull << 30)
#endif
#ifndef GTA_HEAP_BASE
uint8_t* g_w2c_heap;  // the base the engine code adds to every address (builds without a fixed heap address)
#endif

wasm_rt_shared_memory_t g_mem;
_Thread_local w2c_gta* t_inst;
const char* g_data_dir;
const char* g_user_dir;
int g_verbose;

static u64 g_heap_max;
static u32 g_has_window;
static int g_width = 1280, g_height = 720;
static double g_t0;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

// ---- logging ------------------------------------------------------------------------------------------------------

void host_log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  pthread_mutex_lock(&g_log_mu);
  fprintf(stderr, "[host %8.3f] ", (host_now_ms() - g_t0) / 1000.0);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  pthread_mutex_unlock(&g_log_mu);
  va_end(ap);
}

// Logs the first time `key` is seen: for imports that are stubbed out.
void host_once(const char* key, const char* fmt, ...) {
  static const char* seen[256];
  static int n;
  pthread_mutex_lock(&g_log_mu);
  for (int i = 0; i < n; i++)
    if (!strcmp(seen[i], key)) { pthread_mutex_unlock(&g_log_mu); return; }
  if (n < 256) seen[n++] = strdup(key);
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "[host %8.3f] ", (host_now_ms() - g_t0) / 1000.0);
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
  va_end(ap);
  pthread_mutex_unlock(&g_log_mu);
}

double host_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

double host_epoch_ms(void) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

// A wasm trap (unreachable, out-of-bounds access, bad indirect call...) in engine code: report where, then stop.
void host_trap(wasm_rt_trap_t code) {
  void* frames[24];
  int n = backtrace(frames, 24);
  pthread_mutex_lock(&g_log_mu);
  fprintf(stderr, "[host] engine trap: %s\n", wasm_rt_strerror(code));
  backtrace_symbols_fd(frames, n, 2);
  pthread_mutex_unlock(&g_log_mu);
  fflush(NULL);
  abort();
}

// Writes the call stack of every thread of the process to the log (walks the frame-pointer chains): what a stuck game is doing.
void host_dump_threads(void) {
  thread_act_array_t threads;
  mach_msg_type_number_t count;
  if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
  thread_t self = mach_thread_self();
  pthread_mutex_lock(&g_log_mu);
  for (mach_msg_type_number_t i = 0; i < count; i++) {
    char name[64] = "";
    pthread_t pt = pthread_from_mach_thread_np(threads[i]);
    if (pt) pthread_getname_np(pt, name, sizeof name);
    fprintf(stderr, "[threads] thread %u %s\n", i, name);
    void* frames[40];
    int n = 0;
    if (threads[i] == self) n = backtrace(frames, 40);
    else if (thread_suspend(threads[i]) == KERN_SUCCESS) {
      arm_thread_state64_t st;
      mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
      if (thread_get_state(threads[i], ARM_THREAD_STATE64, (thread_state_t)&st, &sc) == KERN_SUCCESS) {
        frames[n++] = (void*)arm_thread_state64_get_pc(st);
        uintptr_t fp = arm_thread_state64_get_fp(st);
        // each frame record is {previous fp, return address}; stop at anything that does not look like a stack address going up
        while (n < 40 && fp && !(fp & 7)) {
          uintptr_t rec[2];
          vm_size_t got = 0;
          if (vm_read_overwrite(mach_task_self(), fp, sizeof rec, (vm_address_t)rec, &got) != KERN_SUCCESS || got != sizeof rec) break;
          if (!rec[1]) break;
          frames[n++] = (void*)(rec[1] & 0x0000007fffffffffull);  // strip pointer-authentication bits
          if (rec[0] <= fp) break;
          fp = rec[0];
        }
      }
      thread_resume(threads[i]);
    }
    for (int k = 0; k < n; k++) {
      Dl_info info;
      if (dladdr(frames[k], &info) && info.dli_sname) fprintf(stderr, "[threads]   %.150s + %ld\n", info.dli_sname, (long)((char*)frames[k] - (char*)info.dli_saddr));
      else fprintf(stderr, "[threads]   %p\n", frames[k]);
    }
    mach_port_deallocate(mach_task_self(), threads[i]);
  }
  pthread_mutex_unlock(&g_log_mu);
  mach_port_deallocate(mach_task_self(), self);
  vm_deallocate(mach_task_self(), (vm_address_t)threads, count * sizeof *threads);
  fflush(NULL);
}

// ---- linear memory ------------------------------------------------------------------------------------------------

// The module declares a shared 64-bit memory, 3 GiB initially and 16 GiB at most. The whole range the host allows is
// reserved once so the base never moves; untouched pages cost nothing.
void host_memory_init(void) {
  const char* cap = getenv("GTA_HEAP_MAX_MB");
  g_heap_max = cap ? strtoull(cap, NULL, 10) << 20 : DEFAULT_HEAP_MAX;
  u64 initial = 49152 * PAGE;
  if (g_heap_max < initial) g_heap_max = initial;
#ifdef GTA_HEAP_BASE
  // At the address the engine code was compiled for. VM_FLAGS_FIXED without overwrite: it fails rather than replace something
  // already mapped there.
  mach_vm_address_t at = GTA_HEAP_BASE;
  kern_return_t kr = mach_vm_allocate(mach_task_self(), &at, g_heap_max, VM_FLAGS_FIXED);
  if (kr != KERN_SUCCESS) { fprintf(stderr, "the engine heap could not be mapped at %p: %s\n", (void*)GTA_HEAP_BASE, mach_error_string(kr)); exit(1); }
  void* p = (void*)at;
#else
  void* p = mmap(NULL, g_heap_max, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) { perror("reserving the engine heap"); exit(1); }
  g_w2c_heap = p;
#endif
  g_mem.data = (_Atomic volatile uint8_t*)p;
  g_mem.data_end = (_Atomic volatile uint8_t*)p + g_heap_max;
  g_mem.page_size = PAGE;
  g_mem.pages = 49152;
  g_mem.max_pages = g_heap_max / PAGE;
  g_mem.size = initial;
  g_mem.is64 = true;
  pthread_mutex_init(&g_mem.mem_lock, NULL);
}

wasm_rt_shared_memory_t* w2c_env_memory(struct w2c_env* env) { return &g_mem; }

uint64_t wasm_rt_grow_memory_shared(wasm_rt_shared_memory_t* m, uint64_t delta) {
  pthread_mutex_lock(&m->mem_lock);
  uint64_t old = m->pages, ret = (uint64_t)-1;
  if (old + delta <= m->max_pages) {
    m->pages = old + delta;
    m->size = m->pages * PAGE;
    ret = old;
  }
  pthread_mutex_unlock(&m->mem_lock);
  return ret;
}

u64 w2c_env_emscripten_get_heap_max(struct w2c_env* env) { return g_heap_max; }

u32 w2c_env_emscripten_resize_heap(struct w2c_env* env, u64 requested) {
  u64 old = g_mem.size;
  if (requested <= old) return 0;
  if (requested > g_heap_max) return 0;
  u64 over = old + old / 5;
  if (over > requested + 100663296) over = requested + 100663296;
  u64 want = requested > over ? requested : over;
  want = (want + PAGE - 1) / PAGE * PAGE;
  if (want > g_heap_max) want = g_heap_max;
  if (wasm_rt_grow_memory_shared(&g_mem, (want - old) / PAGE) == (uint64_t)-1) return 0;
  host_log("heap grown to %llu MB", (unsigned long long)(want >> 20));
  return 1;
}

// One line splitting the app's memory footprint into the engine's heap and everything else (renderer, Dawn, stacks, code).
// Heap pages count when resident or held by the compressor, which is what the footprint counts too.
void host_log_memory(void) {
  task_vm_info_data_t info;
  mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n) != KERN_SUCCESS) return;
  u64 pages = 0;
  vm_address_t at = (vm_address_t)g_mem.data, end = at + g_heap_max;
  while (at < end) {
    vm_size_t size = 0;
    natural_t depth = 64;
    vm_region_submap_info_data_64_t r;
    mach_msg_type_number_t rn = VM_REGION_SUBMAP_INFO_COUNT_64;
    if (vm_region_recurse_64(mach_task_self(), &at, &size, &depth, (vm_region_recurse_info_t)&r, &rn) != KERN_SUCCESS || at >= end) break;
    pages += r.pages_resident + r.pages_swapped_out;
    at += size;
  }
  u64 heap = pages * vm_kernel_page_size, total = info.phys_footprint;
  host_log("memory: app footprint %llu MB = engine heap %llu MB (of %llu MB claimed) + everything else %llu MB", (unsigned long long)(total >> 20),
           (unsigned long long)(heap >> 20), (unsigned long long)(g_mem.size >> 20), (unsigned long long)((total > heap ? total - heap : 0) >> 20));
}

// ---- futex (memory.atomic.wait / notify) ------------------------------------------------------------------------------

typedef struct waiter {
  u64 addr;
  pthread_cond_t cv;
  int woken;
  struct waiter* next;
} waiter;

#define BUCKETS 256
static pthread_mutex_t fb_mu[BUCKETS];
static waiter* fb_head[BUCKETS];

static void futex_init(void) {
  for (int i = 0; i < BUCKETS; i++) pthread_mutex_init(&fb_mu[i], NULL);
}

static u32 futex_wait(u64 addr, bool equal, int64_t timeout_ns, int b) {
  // called with fb_mu[b] held
  if (!equal) { pthread_mutex_unlock(&fb_mu[b]); return 1; }
  waiter w = { .addr = addr, .woken = 0, .next = fb_head[b] };
  pthread_cond_init(&w.cv, NULL);
  fb_head[b] = &w;
  if (timeout_ns < 0) {
    while (!w.woken) pthread_cond_wait(&w.cv, &fb_mu[b]);
  } else {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ns / 1000000000;
    ts.tv_nsec += timeout_ns % 1000000000;
    if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    while (!w.woken && pthread_cond_timedwait(&w.cv, &fb_mu[b], &ts) != ETIMEDOUT) {}
  }
  if (!w.woken)
    for (waiter** p = &fb_head[b]; *p; p = &(*p)->next)
      if (*p == &w) { *p = w.next; break; }
  pthread_mutex_unlock(&fb_mu[b]);
  pthread_cond_destroy(&w.cv);
  return w.woken ? 0 : 2;
}

static int bucket_of(u64 addr) { return (int)((addr >> 2) * 2654435761u % BUCKETS); }

u32 w2c_host_atomic_wait32(wasm_rt_shared_memory_t* m, u64 addr, u32 expected, u64 timeout_ns) {
  int b = bucket_of(addr);
  pthread_mutex_lock(&fb_mu[b]);
  return futex_wait(addr, atomic_load((_Atomic u32*)gptr(addr)) == expected, (int64_t)timeout_ns, b);
}

u32 w2c_host_atomic_wait64(wasm_rt_shared_memory_t* m, u64 addr, u64 expected, u64 timeout_ns) {
  int b = bucket_of(addr);
  pthread_mutex_lock(&fb_mu[b]);
  return futex_wait(addr, atomic_load((_Atomic u64*)gptr(addr)) == expected, (int64_t)timeout_ns, b);
}

u32 w2c_host_atomic_notify(wasm_rt_shared_memory_t* m, u64 addr, u32 count) {
  int b = bucket_of(addr);
  u32 n = 0;
  pthread_mutex_lock(&fb_mu[b]);
  for (waiter** p = &fb_head[b]; *p && n < count;) {
    waiter* w = *p;
    if (w->addr == addr) {
      *p = w->next;
      w->woken = 1;
      pthread_cond_signal(&w->cv);
      n++;
    } else {
      p = &w->next;
    }
  }
  pthread_mutex_unlock(&fb_mu[b]);
  return n;
}

// ---- threads --------------------------------------------------------------------------------------------------------

typedef struct {
  u64 pthread_ptr, start, arg;
} thread_start;

static w2c_gta* new_instance(void) {
  w2c_gta* inst = calloc(1, sizeof *inst);
  struct w2c_env* env = calloc(1, sizeof *env);
  env->inst = inst;
  t_inst = inst;
  wasm2c_gta_instantiate(inst, env, (struct w2c_wasi__snapshot__preview1*)env);
  return inst;
}

static wasm_rt_funcref_t table_entry(u64 index) {
  wasm_rt_funcref_table_t* t = &t_inst->w2c_0x5F_indirect_function_table;
  if (index >= t->size || !t->data[index].func) {
    host_log("call through an empty function table entry %llu", (unsigned long long)index);
    abort();
  }
  return t->data[index];
}

static void* thread_main(void* p) {
  thread_start s = *(thread_start*)p;
  free(p);
  wasm_rt_init_thread();
  w2c_gta* inst = new_instance();
  // game.js establishStackSpace: the stack the creator allocated is recorded in the pthread struct
  u64 high = rd64(s.pthread_ptr + 80), size = rd64(s.pthread_ptr + 88);
  w2c_gta_emscripten_stack_set_limits(inst, high, high - size);
  w2c_gta_0x5Femscripten_stack_restore(inst, high);
  w2c_gta_0x5Femscripten_thread_init(inst, s.pthread_ptr, 0, 0, 1, 0, 0);
  w2c_gta_0x5Femscripten_tls_init(inst);
  wasm_rt_funcref_t f = table_entry(s.start);
  u64 result = ((u64 (*)(void*, u64))f.func)(f.module_instance, s.arg);
  w2c_gta_0x5Femscripten_thread_exit(inst, result);
  return NULL;
}

static void spawn(void* (*fn)(void*), void* arg, size_t stack) {
  pthread_attr_t a;
  pthread_attr_init(&a);
  pthread_attr_setstacksize(&a, stack);
  pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);
  pthread_t t;
  int e = pthread_create(&t, &a, fn, arg);
  if (e) { host_log("pthread_create failed: %s", strerror(e)); abort(); }
  pthread_attr_destroy(&a);
}

u32 w2c_env_0x5F_pthread_create_js(struct w2c_env* env, u64 pthread_ptr, u64 attr, u64 start, u64 arg) {
  thread_start* s = malloc(sizeof *s);
  *s = (thread_start){ pthread_ptr, start, arg };
  spawn(thread_main, s, THREAD_STACK);
  return 0;
}

void w2c_env_0x5Femscripten_init_main_thread_js(struct w2c_env* env, u64 tb) {
  // the engine runs in a worker in the browser: not the browser main thread, the runtime thread, allowed to block
  w2c_gta_0x5Femscripten_thread_init(env->inst, tb, 0, 1, 1, 1048576, 0);
  w2c_gta_0x5Femscripten_tls_init(env->inst);
}

// A thread leaves through here when it exits below its entry point (pthread_exit).
void w2c_env_emscripten_exit_with_live_runtime(struct w2c_env* env) { pthread_exit(NULL); }

void w2c_env_0x5Femscripten_thread_cleanup(struct w2c_env* env, u64 thread) {}
void w2c_env_0x5Femscripten_thread_set_strongref(struct w2c_env* env, u64 thread) {}
void w2c_env_0x5Femscripten_thread_mailbox_await(struct w2c_env* env, u64 thread) {}
void w2c_env_emscripten_check_blocking_allowed(struct w2c_env* env) {}

void w2c_env_0x5Femscripten_notify_mailbox_postmessage(struct w2c_env* env, u64 target, u64 current) {
  host_once("mailbox", "unhandled: a thread posted work to another thread's mailbox (target %llx)", (unsigned long long)target);
}

f64 w2c_env_0x5Femscripten_receive_on_main_thread_js(struct w2c_env* env, u32 func, u64 em_asm, u64 thread, u32 size, u64 args, u64 ctx, u64 ctx_args) {
  host_once("proxy", "unhandled: JS call proxied to the main thread (func %u)", func);
  return 0;
}

// ---- setjmp/longjmp through invoke_* --------------------------------------------------------------------------------------

static _Thread_local jmp_buf* t_jmp;

void w2c_env_0x5Femscripten_throw_longjmp(struct w2c_env* env) {
  if (!t_jmp) { host_log("longjmp with no invoke frame"); abort(); }
  longjmp(*t_jmp, 1);
}

#define INVOKE_ENTER                                           \
  u64 sp = w2c_gta_emscripten_stack_get_current(env->inst);    \
  jmp_buf jb, *prev = t_jmp;                                   \
  wasm_rt_funcref_t f = table_entry(index);                    \
  t_jmp = &jb;                                                 \
  if (!setjmp(jb)) {
#define INVOKE_LEAVE(fail)                                     \
  }                                                            \
  t_jmp = prev;                                                \
  w2c_gta_0x5Femscripten_stack_restore(env->inst, sp);         \
  w2c_gta_setThrew(env->inst, 1, 0);                           \
  return fail;
#define CALL(ret, ...) ((ret (*)(void*, ##__VA_ARGS__))f.func)

void w2c_env_invoke_vj(struct w2c_env* env, u64 index, u64 a1) {
  INVOKE_ENTER CALL(void, u64)(f.module_instance, a1); t_jmp = prev; return; INVOKE_LEAVE()
}
void w2c_env_invoke_vjj(struct w2c_env* env, u64 index, u64 a1, u64 a2) {
  INVOKE_ENTER CALL(void, u64, u64)(f.module_instance, a1, a2); t_jmp = prev; return; INVOKE_LEAVE()
}
void w2c_env_invoke_vjii(struct w2c_env* env, u64 index, u64 a1, u32 a2, u32 a3) {
  INVOKE_ENTER CALL(void, u64, u32, u32)(f.module_instance, a1, a2, a3); t_jmp = prev; return; INVOKE_LEAVE()
}
void w2c_env_invoke_vjij(struct w2c_env* env, u64 index, u64 a1, u32 a2, u64 a3) {
  INVOKE_ENTER CALL(void, u64, u32, u64)(f.module_instance, a1, a2, a3); t_jmp = prev; return; INVOKE_LEAVE()
}
void w2c_env_invoke_vjjii(struct w2c_env* env, u64 index, u64 a1, u64 a2, u32 a3, u32 a4) {
  INVOKE_ENTER CALL(void, u64, u64, u32, u32)(f.module_instance, a1, a2, a3, a4); t_jmp = prev; return; INVOKE_LEAVE()
}
u32 w2c_env_invoke_ij(struct w2c_env* env, u64 index, u64 a1) {
  INVOKE_ENTER u32 r = CALL(u32, u64)(f.module_instance, a1); t_jmp = prev; return r; INVOKE_LEAVE(0)
}
u32 w2c_env_invoke_iji(struct w2c_env* env, u64 index, u64 a1, u32 a2) {
  INVOKE_ENTER u32 r = CALL(u32, u64, u32)(f.module_instance, a1, a2); t_jmp = prev; return r; INVOKE_LEAVE(0)
}
u32 w2c_env_invoke_ijji(struct w2c_env* env, u64 index, u64 a1, u64 a2, u32 a3) {
  INVOKE_ENTER u32 r = CALL(u32, u64, u64, u32)(f.module_instance, a1, a2, a3); t_jmp = prev; return r; INVOKE_LEAVE(0)
}
u32 w2c_env_invoke_ijjiii(struct w2c_env* env, u64 index, u64 a1, u64 a2, u32 a3, u32 a4, u32 a5) {
  INVOKE_ENTER u32 r = CALL(u32, u64, u64, u32, u32, u32)(f.module_instance, a1, a2, a3, a4, a5); t_jmp = prev; return r; INVOKE_LEAVE(0)
}
u64 w2c_env_invoke_jjij(struct w2c_env* env, u64 index, u64 a1, u32 a2, u64 a3) {
  INVOKE_ENTER u64 r = CALL(u64, u64, u32, u64)(f.module_instance, a1, a2, a3); t_jmp = prev; return r; INVOKE_LEAVE(0)
}
u64 w2c_env_invoke_jjiiijij(struct w2c_env* env, u64 index, u64 a1, u32 a2, u32 a3, u32 a4, u64 a5, u32 a6, u64 a7) {
  INVOKE_ENTER u64 r = CALL(u64, u64, u32, u32, u32, u64, u32, u64)(f.module_instance, a1, a2, a3, a4, a5, a6, a7); t_jmp = prev; return r; INVOKE_LEAVE(0)
}

// ---- time ---------------------------------------------------------------------------------------------------------

f64 w2c_env_emscripten_get_now(struct w2c_env* env) { return host_now_ms() - g_t0; }
f64 w2c_env_emscripten_date_now(struct w2c_env* env) { return host_epoch_ms(); }
f64 w2c_env_wasm_now_ms(struct w2c_env* env) { return host_epoch_ms(); }

u32 w2c_wasi__snapshot__preview1_clock_time_get(struct w2c_wasi__snapshot__preview1* w, u32 clock, u64 precision, u64 out) {
  struct timespec ts;
  clock_gettime(clock == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
  wr64(out, (u64)ts.tv_sec * 1000000000ull + ts.tv_nsec);
  return 0;
}

static void put_tm(u64 p, const struct tm* t) {
  wr32(p, t->tm_sec); wr32(p + 4, t->tm_min); wr32(p + 8, t->tm_hour); wr32(p + 12, t->tm_mday);
  wr32(p + 16, t->tm_mon); wr32(p + 20, t->tm_year); wr32(p + 24, t->tm_wday); wr32(p + 28, t->tm_yday);
  wr32(p + 32, t->tm_isdst > 0);
  wr64(p + 40, (u64)(int64_t)t->tm_gmtoff);
}

u32 w2c_env_0x5Flocaltime_js(struct w2c_env* env, u64 time, u64 tm_ptr) {
  time_t t = (time_t)(int64_t)time;
  struct tm tm;
  localtime_r(&t, &tm);
  put_tm(tm_ptr, &tm);
  return 0;
}

u32 w2c_env_0x5Fgmtime_js(struct w2c_env* env, u64 time, u64 tm_ptr) {
  time_t t = (time_t)(int64_t)time;
  struct tm tm;
  gmtime_r(&t, &tm);
  put_tm(tm_ptr, &tm);
  return 0;
}

u64 w2c_env_0x5Fmktime_js(struct w2c_env* env, u64 p) {
  struct tm tm = { .tm_sec = rd32(p), .tm_min = rd32(p + 4), .tm_hour = rd32(p + 8), .tm_mday = rd32(p + 12),
                   .tm_mon = rd32(p + 16), .tm_year = rd32(p + 20), .tm_isdst = -1 };
  time_t t = mktime(&tm);
  if (t != (time_t)-1) put_tm(p, &tm);
  return (u64)(int64_t)t;
}

void w2c_env_0x5Ftzset_js(struct w2c_env* env, u64 timezone_p, u64 daylight_p, u64 std_name, u64 dst_name) {
  tzset();
  wr64(timezone_p, (u64)(int64_t)timezone);
  wr32(daylight_p, daylight ? 1 : 0);
  snprintf(gptr(std_name), 17, "%s", tzname[0]);
  snprintf(gptr(dst_name), 17, "%s", tzname[1]);
}

// ---- process, diagnostics, page glue ------------------------------------------------------------------------------------

void w2c_env_exit(struct w2c_env* env, u32 code) { host_log("engine exit(%u)", code); fflush(NULL); _exit(code); }
void w2c_wasi__snapshot__preview1_proc_exit(struct w2c_wasi__snapshot__preview1* w, u32 code) { host_log("engine proc_exit(%u)", code); fflush(NULL); _exit(code); }
void w2c_env_0x5Fabort_js(struct w2c_env* env) { host_log("engine abort()"); fflush(NULL); abort(); }
u32 w2c_env_0x5Femscripten_system(struct w2c_env* env, u64 command) { return command ? (u32)-52 : 0; }
u64 w2c_env_emscripten_stack_snapshot(struct w2c_env* env) { return 0; }
u32 w2c_env_emscripten_stack_unwind_buffer(struct w2c_env* env, u64 pc, u64 buf, u32 count) { return 0; }
u32 w2c_env_emscripten_num_logical_cores(struct w2c_env* env) {
  const char* n = getenv("GTA_CORES");
  return n ? (u32)atoi(n) : (u32)sysconf(_SC_NPROCESSORS_ONLN);
}

static void print_line(const char* tag, u64 s) {
  pthread_mutex_lock(&g_log_mu);
  fprintf(stdout, "%s%s\n", tag, (const char*)gptr(s));
  fflush(stdout);
  pthread_mutex_unlock(&g_log_mu);
}
void w2c_env_wasm_print_line_js(struct w2c_env* env, u64 s) { print_line("", s); }
void w2c_env_wasm_hang_line_js(struct w2c_env* env, u64 s) { print_line("[hang] ", s); }

// Module options the engine asks the page for (loader.js `Module`); GTA_OPT_<name>=N overrides the default.
u32 w2c_env_wasm_module_int_js(struct w2c_env* env, u64 name, u32 default_value) {
  char key[96];
  snprintf(key, sizeof key, "GTA_OPT_%s", (const char*)gptr(name));
  const char* v = getenv(key);
  host_once(key, "module option %s = %d%s", (const char*)gptr(name), v ? atoi(v) : (int)default_value, v ? "" : " (default)");
  return v ? (u32)atoi(v) : default_value;
}

// 1 = running in a page (audio, input and the progress UI exist), 0 = the headless Node configuration.
u32 w2c_env_wasm_has_page_js(struct w2c_env* env) {
  const char* v = getenv("GTA_PAGE");
  return v ? (u32)atoi(v) : g_has_window;
}

void w2c_env_wasm_audio_publish_js(struct w2c_env* env, u64 ring, u32 capacity) {
  if (getenv("GTA_AUDIO") && !atoi(getenv("GTA_AUDIO"))) return;
  host_audio_start(gptr(ring), capacity);
}
// GTA_AUTOPLAY=1 (test aid): a scripted player that runs, turns, jumps and shoots, to load the engine the way real play does
// when nobody is at the controls.
static void* autoplay_main(void* p) {
  usleep(30 * 1000000);  // the world loads first
  host_log("autoplay: started");
  unsigned seed = 12345;
  for (int tick = 0;; tick++) {
    host_input_key('W', 1);
    host_input_key(0xA0, tick % 200 < 150);                      // sprint most of the time
    if (tick % 40 == 0) host_input_mouse_delta((int)(rand_r(&seed) % 600) - 300, (int)(rand_r(&seed) % 60) - 30);
    if (tick % 100 == 10) host_input_button(0, 1);               // hold fire for a second
    if (tick % 100 == 30) host_input_button(0, 0);
    if (tick % 140 == 70) host_input_key(0x20, 1);               // jump
    if (tick % 140 == 73) host_input_key(0x20, 0);
    if (tick % 600 == 300) host_input_wheel(1);                  // next weapon
    usleep(50000);
  }
  return NULL;
}

void w2c_env_wasm_input_publish_js(struct w2c_env* env, u64 block) {
  // GTA_MODE: story | sandbox (the GTA V map, the default) | sandbox6 (the env_test map)
  const char* mode = getenv("GTA_MODE");
  int start = mode && !strcmp(mode, "story") ? 1 : mode && !strcmp(mode, "sandbox6") ? 3 : 2;
  host_input_attach(gptr(block), g_width, g_height, start);
  host_log("input block at %llx, start mode %d", (unsigned long long)block, start);
  if (getenv("GTA_AUTOPLAY")) { pthread_t t; pthread_create(&t, NULL, autoplay_main, NULL); pthread_detach(t); }
}

// ---- renderer glue ------------------------------------------------------------------------------------------------------

static const char* g_gpu_mode;  // "dawn" (default), "null" (consume the commands, draw nothing) or "off" (engine without -wgpu)
static char g_shader_dir[2048];

uint8_t* host_mem_base(void) { return MEM; }
uint32_t host_futex_wait32(uint64_t addr, uint32_t expected, int64_t timeout_ns) { return w2c_host_atomic_wait32(&g_mem, addr, expected, (u64)timeout_ns); }
uint32_t host_futex_notify(uint64_t addr, uint32_t count) { return w2c_host_atomic_notify(&g_mem, addr, count); }
const char* host_shader_dir(void) { return g_shader_dir; }
const char* host_user_dir(void) { return g_user_dir; }

// The translated shaders sit next to the data in the mirror: <mirror>/b/<build>/shaders. GTA_SHADERS overrides.
static void find_shader_dir(const char* given) {
  if (given) { snprintf(g_shader_dir, sizeof g_shader_dir, "%s", given); return; }
  char pattern[2048];
  snprintf(pattern, sizeof pattern, "%s/../b/*/shaders", g_data_dir);
  glob_t g;
  if (!glob(pattern, 0, NULL, &g) && g.gl_pathc) snprintf(g_shader_dir, sizeof g_shader_dir, "%s", g.gl_pathv[0]);
  globfree(&g);
}

void w2c_env_wgpu_start_worker(struct w2c_env* env, u64 ring, u64 size, u64 dbg) {
  if (!strcmp(g_gpu_mode, "null")) gpu_null_start(ring, size, dbg);
  else gpu_start(ring, size, dbg);
}

// EM_ASM blocks of the page build, by their address in the data section (game.js ASM_CONSTS).
static u32 asm_const(u64 code, u64 sig, u64 buf) {
  u64 a[4] = { 0 };
  int n = 0;
  for (char ch; (ch = *(char*)gptr(sig++)) && n < 4;) {
    bool wide = ch != 'i';
    if (wide && buf % 8) buf += 4;
    a[n++] = wide ? rd64(buf) : rd32(buf);
    buf += wide ? 8 : 4;
  }
  switch (code) {
    case 7980558: snprintf(gptr(a[0]), a[1], "en-US"); return 0;  // navigator.language
    case 7980673: host_log("engine asked to open %s", (const char*)gptr(a[0])); return 0;  // window.open
    case 7980768: host_log("engine asked for a page reload"); return 0;
  }
  host_log("unknown EM_ASM block %llu", (unsigned long long)code);
  return 0;
}
u32 w2c_env_emscripten_asm_const_int(struct w2c_env* env, u64 code, u64 sig, u64 buf) { return asm_const(code, sig, buf); }
u32 w2c_env_emscripten_asm_const_int_sync_on_main_thread(struct w2c_env* env, u64 code, u64 sig, u64 buf) { return asm_const(code, sig, buf); }

// ---- main ---------------------------------------------------------------------------------------------------------

typedef struct {
  int argc;
  char** argv;
} engine_args;

static void* engine_main(void* p) {
  engine_args* ea = p;
  wasm_rt_init_thread();
  w2c_gta* inst = new_instance();
  w2c_gta_0x5F_wasm_call_ctors(inst);
  // game.js callMain: argv and its strings go on the engine's stack
  u64 argv = w2c_gta_0x5Femscripten_stack_alloc(inst, (u64)(ea->argc + 1) * 8);
  for (int i = 0; i < ea->argc; i++) {
    size_t n = strlen(ea->argv[i]) + 1;
    u64 s = w2c_gta_0x5Femscripten_stack_alloc(inst, n);
    memcpy(gptr(s), ea->argv[i], n);
    wr64(argv + 8 * (u64)i, s);
  }
  wr64(argv + 8 * (u64)ea->argc, 0);
  host_log("starting the engine");
  u32 ret = w2c_gta_0x5F_main_argc_argv(inst, (u32)ea->argc, argv);
  host_log("engine main returned %u", ret);
  fflush(NULL);
  _exit((int)ret);
}

void host_start(const host_config* c) {
  g_t0 = host_now_ms();
  g_data_dir = strdup(c->data_dir);
  g_user_dir = strdup(c->user_dir);
  g_verbose = getenv("GTA_VERBOSE") ? atoi(getenv("GTA_VERBOSE")) : 0;
  g_gpu_mode = c->gpu_mode ? c->gpu_mode : "dawn";
  g_has_window = c->window;
  g_width = c->width; g_height = c->height;
  find_shader_dir(c->shader_dir);
  // index.html `args`
  static char width[32], height[32];
  snprintf(width, sizeof width, "-width=%d", c->width);
  snprintf(height, sizeof height, "-height=%d", c->height);
  char* base_args[] = { "./this.program", "-rootdir=/game/", "-nodisplaycalibration", "-noSocialClub", "-pc:nosocialclub",
                        "-nonetwork", "-output", width, height, "-forceResolution",
                        "-Script_all=warning", "-replay_all=warning", "-fastscriptcore" };
  int nbase = sizeof base_args / sizeof *base_args;
  engine_args* ea = malloc(sizeof *ea);
  ea->argc = nbase + c->argc;
  ea->argv = malloc(sizeof(char*) * (size_t)(ea->argc + 2));
  memcpy(ea->argv, base_args, sizeof base_args);
  bool limit_given = false;
  for (int i = 0; i < c->argc; i++) {
    ea->argv[nbase + i] = strdup(c->argv[i]);
    if (!strncmp(c->argv[i], "-frameLimit=", 12)) limit_given = true;
  }
  // the game's own limiter: 1 = 60 fps (the page's default), 2 = 30 fps, 0 = uncapped
  if (!limit_given) ea->argv[ea->argc++] = "-frameLimit=1";
  if (strcmp(g_gpu_mode, "off")) ea->argv[ea->argc++] = "-wgpu";

  // the engine keeps over a thousand archives open
  struct rlimit nofile;
  if (!getrlimit(RLIMIT_NOFILE, &nofile)) {
    rlim_t want = nofile.rlim_max < 8192 ? nofile.rlim_max : 8192;
    if (nofile.rlim_cur < want) { nofile.rlim_cur = want; setrlimit(RLIMIT_NOFILE, &nofile); }
    getrlimit(RLIMIT_NOFILE, &nofile);
    host_log("open file limit %llu", (unsigned long long)nofile.rlim_cur);
  }
  wasm_rt_init();
  host_memory_init();
  futex_init();
  spawn(engine_main, ea, MAIN_STACK);
}

#if !TARGET_OS_IPHONE
int main(int argc, char** argv) {
  host_config c = { .data_dir = getenv("GTA_DATA"), .user_dir = getenv("GTA_USER"), .shader_dir = getenv("GTA_SHADERS"),
                    .gpu_mode = getenv("GTA_GPU") ? getenv("GTA_GPU") : "dawn", .width = 1280, .height = 720, .argc = argc - 1, .argv = argv + 1 };
  if (!c.data_dir || !c.user_dir) {
    fprintf(stderr, "usage: GTA_DATA=<mirror>/data GTA_USER=<writable folder> %s [engine args]\n", argv[0]);
    return 2;
  }
  c.window = !strcmp(c.gpu_mode, "dawn") && !(getenv("GTA_WINDOW") && !atoi(getenv("GTA_WINDOW")));
  host_window_want(c.window);
  host_start(&c);
  if (c.window) host_window_run(c.width, c.height);
  for (;;) pause();
}
#endif
