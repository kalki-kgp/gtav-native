// Native host for the wasm2c-translated engine: replaces the browser glue
// (game.js, loader.js, io_worker.js) with POSIX code.
#ifndef GTA_HOST_H
#define GTA_HOST_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gta.h"

// One per engine thread: every thread runs its own instance of the module
// (own globals and function table) over the one shared linear memory.
struct w2c_env {
  w2c_gta* inst;
};
struct w2c_wasi__snapshot__preview1 {
  w2c_gta* inst;
};

extern wasm_rt_shared_memory_t g_mem;
extern _Thread_local w2c_gta* t_inst;
extern const char* g_data_dir;  // the mirrored /data folder (manifest.json, common/, x64/)
extern const char* g_user_dir;  // host folder backing the engine's writable paths
extern int g_verbose;

#define MEM ((uint8_t*)g_mem.data)
static inline void* gptr(u64 addr) { return MEM + addr; }
static inline u32 rd32(u64 a) { u32 v; memcpy(&v, MEM + a, 4); return v; }
static inline u64 rd64(u64 a) { u64 v; memcpy(&v, MEM + a, 8); return v; }
static inline void wr32(u64 a, u32 v) { memcpy(MEM + a, &v, 4); }
static inline void wr64(u64 a, u64 v) { memcpy(MEM + a, &v, 8); }
static inline _Atomic int32_t* aword(u64 word_index) { return (_Atomic int32_t*)(MEM + word_index * 4); }

void host_memory_init(void);
void host_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void host_once(const char* key, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
double host_now_ms(void);
void host_log_memory(void);
double host_epoch_ms(void);

u32 w2c_host_atomic_wait32(wasm_rt_shared_memory_t*, u64 addr, u32 expected, u64 timeout_ns);
u32 w2c_host_atomic_wait64(wasm_rt_shared_memory_t*, u64 addr, u64 expected, u64 timeout_ns);
u32 w2c_host_atomic_notify(wasm_rt_shared_memory_t*, u64 addr, u32 count);

#endif
