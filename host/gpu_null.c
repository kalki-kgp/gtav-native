// Stand-in for the renderer: consumes the engine's GPU command ring without drawing, and completes the things the engine
// waits for (fences, queries, readbacks). Ring protocol: wgpu_worker.js of the browser build.
#include <pthread.h>
#include <stdlib.h>

#include "gpu_host.h"
#include "host.h"

enum { RING_DATA = 64 };
enum { OP_WRAP = 1, OP_PRESENT = 7, OP_FENCE = 8, OP_LOG = 10, OP_READBACK = 42, OP_CREATE_QUERY = 44, OP_END_QUERY = 46, OP_DRAW = 32 };

typedef struct { u32 id, type; u64 addr; } query;
static query* queries;
static size_t nqueries, capqueries;

static void notify_word(u64 w) { w2c_host_atomic_notify(&g_mem, w * 4, UINT32_MAX); }

static void query_store(const query* q, u64 result, u32 seq) {
  u64 base = q->addr / 4;
  atomic_store(aword(base + 2), (int32_t)(u32)result);
  atomic_store(aword(base + 3), (int32_t)(u32)(result >> 32));
  atomic_store(aword(base), (int32_t)seq);
  notify_word(base);
}

static void* gpu_main(void* arg) {
  u64 ring = (u64)(uintptr_t)arg / 4;  // word index of the ring header: head, tail, state, executed, errors, sleeping
  u32 tail = RING_DATA;
  u64 frames = 0, flast = 0, draws = 0, commands = 0;
  double t0 = host_now_ms(), tlast = t0;
  atomic_store(aword(ring + 1), (int32_t)tail);
  atomic_store(aword(ring + 2), 1);
  for (;;) {
    u32 head = (u32)atomic_load(aword(ring));
    if (head == tail) {
      atomic_store(aword(ring + 5), 1);
      if ((u32)atomic_load(aword(ring)) == tail) w2c_host_atomic_wait32(&g_mem, ring * 4, tail, 100000000ull);
      atomic_store(aword(ring + 5), 0);
      continue;
    }
    while (tail != head) {
      u64 base = ring + (tail >> 2), p = base + 2;
      u32 op = rd32(base * 4), n = rd32(base * 4 + 4);
      if (op == OP_WRAP) { tail = RING_DATA; continue; }
      #define W(i) rd32((p + (i)) * 4)
      switch (op) {
        case OP_FENCE: {
          u64 addr = W(0) | (u64)W(1) << 32;
          atomic_store(aword(addr / 4), (int32_t)W(2));
          notify_word(addr / 4);
          break;
        }
        case OP_READBACK: {
          u64 flag = W(15) | (u64)W(16) << 32;
          if (flag) { atomic_store(aword(flag / 4), 0); notify_word(flag / 4); }
          break;
        }
        case OP_CREATE_QUERY:
          if (nqueries == capqueries) queries = realloc(queries, (capqueries = capqueries ? capqueries * 2 : 256) * sizeof *queries);
          queries[nqueries++] = (query){ W(0), W(1), W(2) | (u64)W(3) << 32 };
          break;
        case OP_END_QUERY:
          for (size_t i = nqueries; i-- > 0;)
            if (queries[i].id == W(0)) {
              u32 type = queries[i].type;  // D3D11_QUERY: 0 event, 1 occlusion, 2 timestamp, 5 occlusion predicate
              query_store(&queries[i], type == 1 || type == 5 ? 65536 : type == 2 ? (u64)(host_now_ms() * 1e6) : 1, W(1));
              break;
            }
          break;
        case OP_LOG:
          host_log("gpu log: %.*s", (int)W(0), (const char*)gptr((p + 1) * 4));
          break;
        case OP_DRAW: draws++; break;
        case OP_PRESENT: {
          frames++;
          double now = host_now_ms();
          if (now - tlast >= 5000) {
            host_log("null renderer: %llu frames (%.1f fps over the last 5 s), %llu draws, %llu commands",
                     (unsigned long long)frames, (frames - flast) * 1000.0 / (now - tlast), (unsigned long long)draws, (unsigned long long)commands);
            tlast = now; flast = frames;
          }
          break;
        }
      }
      #undef W
      commands++;
      tail += n * 4 + 8;
      if (!(commands & 511)) { atomic_store(aword(ring + 1), (int32_t)tail); notify_word(ring + 1); }
    }
    atomic_store(aword(ring + 1), (int32_t)tail);
    notify_word(ring + 1);
  }
  return NULL;
}

void gpu_null_start(uint64_t ring, uint64_t size, uint64_t dbg) {
  host_log("GPU command ring at %llx, %llu bytes: null renderer", (unsigned long long)ring, (unsigned long long)size);
  pthread_t t;
  pthread_create(&t, NULL, gpu_main, (void*)(uintptr_t)ring);
  pthread_detach(t);
}
