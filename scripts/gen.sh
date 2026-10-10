#!/bin/sh
# Translate game.wasm to C with the patched wasm2c, then fix up the generated
# helper header for SIMD on shared memory.
# usage: scripts/gen.sh /path/to/game.wasm
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WASM=$1
OUT=$ROOT/gen
OUTPUTS=${OUTPUTS:-256}

rm -rf "$OUT"
mkdir -p "$OUT"
"$ROOT/tools/wabt/build/wasm2c" --enable-threads -n gta --num-outputs="$OUTPUTS" -o "$OUT/gta.c" "$WASM"

# wasm2c only defines the SIMD memory helpers for unshared memories, and only
# gives some of the call sites a _shared suffix. This module has one memory and
# it is shared, so retarget every SIMD helper at the shared memory type and
# alias the suffixed names.
python3 -I - "$OUT/gta-impl.h" <<'EOF'
import re, sys
path = sys.argv[1]
src = open(path, encoding='utf-8').read()
start = src.index('#define DEFINE_SIMD_LOAD_FUNC(')
end = src.index('// clang-format off', start)
block = src[start:end]
fixed = re.sub(r'wasm_rt_memory_t\*( +)mem', r'wasm_rt_shared_memory_t* mem', block)
fixed = re.sub(r'DEF_MEM_CHECKS(\d)\(name, _,', r'DEF_MEM_CHECKS\1(name, _shared_,', fixed)
assert fixed != block and 'wasm_rt_memory_t' not in fixed
aliases = ''.join('#define %s_shared %s\n' % (n, n) for n in (
    'v128_load', 'v128_store', 'i16x8_load8x8', 'u16x8_load8x8', 'i32x4_load16x4',
    'u32x4_load16x4', 'i64x2_load32x2', 'u64x2_load32x2', 'v128_load8_splat',
    'v128_load16_splat', 'v128_load32_splat', 'v128_load64_splat',
    'v128_load32_zero', 'v128_load64_zero'))
# The patched wasm2c forwards memory.atomic.wait/notify to these host functions.
decls = ('u32 w2c_host_atomic_wait32(wasm_rt_shared_memory_t*, u64, u32, u64);\n'
         'u32 w2c_host_atomic_wait64(wasm_rt_shared_memory_t*, u64, u64, u64);\n'
         'u32 w2c_host_atomic_notify(wasm_rt_shared_memory_t*, u64, u32);\n')
open(path, 'w', encoding='utf-8').write(src[:start] + fixed + decls + aliases + '\n' + src[end:])

# Plain memory accesses. wasm2c reaches a shared memory through `mem->data` (an atomic volatile pointer, reloaded for every
# access) and makes every ordinary load and store a volatile atomic one, which defeats most optimization. The host maps the
# heap once at a fixed address (GTA_HEAP_BASE), so an access becomes a constant base plus the offset; ordinary loads and stores
# become single volatile ones (the wasm atomic instructions stay atomic). GTA_NO_BOUNDS_CHECK drops the per-access bounds check.
src = open(path, encoding='utf-8').read()
old = '#define MEM_ADDR(mem, addr, n) &((mem)->data[addr])'
assert src.count(old) == 1
src = src.replace(old, """#ifdef GTA_HEAP_BASE
#define W2C_HEAP ((uint8_t*)GTA_HEAP_BASE)
#else
extern uint8_t* g_w2c_heap;
#define W2C_HEAP g_w2c_heap
#endif
#define MEM_ADDR(mem, addr, n) (W2C_HEAP + (addr))
// One machine load or store per wasm load or store, at any alignment. Volatile, not plain: other engine threads write this
// memory, and wasm guarantees every load really reads it. With plain accesses the C compiler may assume nothing else writes
// and hoist a load out of a loop that waits for another thread, or read a value twice where the engine read it once.
#define W2C_SHARED_AT(t, p) (((volatile struct __attribute__((packed)) { t v; }*)(p))->v)
// GTA_ORDERED_MEMORY: the engine's lock-free code was written for x86, where every store becomes visible to other cores in
// program order and loads are ordered too (TSO). ARM gives no such order to plain accesses, and on the iPhone the job queues
// lost and corrupted entries. With this, aligned loads are acquire loads and aligned stores are release stores, which restores
// that order (unaligned ones stay plain: the ordered instructions fault on them).
#ifdef GTA_ORDERED_MEMORY
#define W2C_SHARED_LOAD(dst, t, p)                                                                    \\
  do {                                                                                                \\
    uint8_t* w2c_p = (p);                                                                             \\
    if (__builtin_expect((uintptr_t)w2c_p & (sizeof(t) - 1), 0)) dst = W2C_SHARED_AT(t, w2c_p);       \\
    else dst = atomic_load_explicit((_Atomic volatile t*)w2c_p, memory_order_acquire);                \\
  } while (0)
#define W2C_SHARED_STORE(t, p, value)                                                                 \\
  do {                                                                                                \\
    uint8_t* w2c_p = (p);                                                                             \\
    if (__builtin_expect((uintptr_t)w2c_p & (sizeof(t) - 1), 0)) W2C_SHARED_AT(t, w2c_p) = (value);   \\
    else atomic_store_explicit((_Atomic volatile t*)w2c_p, (value), memory_order_release);            \\
  } while (0)
#else
#define W2C_SHARED_LOAD(dst, t, p) dst = W2C_SHARED_AT(t, p)
#define W2C_SHARED_STORE(t, p, value) W2C_SHARED_AT(t, p) = (value)
#endif""")
old = '#define RANGE_CHECK(mem, offset, len)            \\\n'
assert src.count(old) == 1
src = src.replace(old, '#ifdef GTA_NO_BOUNDS_CHECK\n#define RANGE_CHECK_ON(mem, offset, len)\n#define RANGE_CHECK(mem, offset, len)\n#else\n#define RANGE_CHECK_ON(mem, offset, len) RANGE_CHECK(mem, offset, len)\n' + old)
end = src.index('} while (0);', src.index(old)) + len('} while (0);')
src = src[:end] + '\n#endif' + src[end:]
old = """    result = atomic_load_explicit(                                            \\
        (_Atomic volatile t1*)MEM_ADDR(mem, addr, sizeof(t1)),                \\
        memory_order_relaxed);                                                \\
"""
assert src.count(old) == 1
src = src.replace(old, "    W2C_SHARED_LOAD(result, t1, MEM_ADDR(mem, addr, sizeof(t1)));            \\\n")
old = """    atomic_store_explicit(                                                    \\
        (_Atomic volatile t1*)MEM_ADDR(mem, addr, sizeof(t1)), wrapped,       \\
        memory_order_relaxed);                                                \\
"""
assert src.count(old) == 1
src = src.replace(old, "    W2C_SHARED_STORE(t1, MEM_ADDR(mem, addr, sizeof(t1)), wrapped);          \\\n")
open(path, 'w', encoding='utf-8').write(src)

# __wasm_call_ctors (a 227,000-line function) and __wasm_init_memory run once and are too large to optimize in reasonable
# time: they alone are left unoptimized, so the file they share with ordinary engine code builds like every other file.
import glob, os
heads = ('void w2c_gta_0x5F_wasm_call_ctors_0(w2c_gta* instance) {\n', 'void w2c_gta_0x5F_wasm_init_memory(w2c_gta* instance) {\n')
marked = 0
for f in glob.glob(os.path.join(os.path.dirname(path), 'gta_*.c')):
    s = open(f, encoding='utf-8').read()
    t = s
    for h in heads:
        t = t.replace('\n' + h, '\n__attribute__((optnone)) ' + h)
        if t.startswith(h): t = '__attribute__((optnone)) ' + t
    if t != s:
        marked += t.count('__attribute__((optnone)) void w2c_gta_0x5F_wasm_')
        open(f, 'w', encoding='utf-8').write(t)
assert marked == 2, marked
EOF
echo "generated $(ls "$OUT"/gta_*.c | wc -l | tr -d ' ') C files in $OUT"
