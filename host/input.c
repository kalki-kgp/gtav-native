// The engine's input block in its heap (WasmInputBlock; the layout the browser page's index.html writes), shared by the
// platform front ends:
//   256 key bytes indexed by Windows virtual-key code (0x80 = held), then int32 words: mouseX, mouseY, dX, dY, dZ, buttons, focus,
//   locked, textHead, textTail; text[64] (uint16) at +296; at +424: dbgDrawLimit, dbgFrameDraws, winW, winH; at +440: startMode.
#include "gpu_host.h"

static uint8_t* block;
static int width, height;
enum { MX, MY, DX, DY, DZ, BTN, FOCUS, LOCKED, HEAD, TAIL };

static int32_t* word(int i) { return (int32_t*)(block + 256 + 4 * i); }
static int32_t wget(int i) { return __atomic_load_n(word(i), __ATOMIC_SEQ_CST); }
static void wset(int i, int32_t v) { if (block) __atomic_store_n(word(i), v, __ATOMIC_SEQ_CST); }
static void wadd(int i, int32_t v) { if (block) __atomic_fetch_add(word(i), v, __ATOMIC_SEQ_CST); }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

void host_input_attach(uint8_t* b, int w, int h, int startMode) {
  width = w; height = h;
  int32_t* ext = (int32_t*)(b + 424);
  __atomic_store_n(&ext[2], w, __ATOMIC_SEQ_CST);
  __atomic_store_n(&ext[3], h, __ATOMIC_SEQ_CST);
  if (startMode) __atomic_store_n((int32_t*)(b + 440), startMode, __ATOMIC_SEQ_CST);
  block = b;
  wset(MX, w / 2); wset(MY, h / 2);
  wset(FOCUS, 1);
}

int host_input_width(void) { return width; }
int host_input_height(void) { return height; }

void host_input_key(int vk, int down) {
  if (block && vk > 0 && vk < 256) __atomic_store_n(&block[vk], down ? 0x80 : 0, __ATOMIC_SEQ_CST);
}

void host_input_button(int bit, int down) {
  if (!block) return;
  if (down) __atomic_fetch_or(word(BTN), 1 << bit, __ATOMIC_SEQ_CST);
  else __atomic_fetch_and(word(BTN), ~(1 << bit), __ATOMIC_SEQ_CST);
}

void host_input_mouse_delta(int dx, int dy) {
  if (!block) return;
  wadd(DX, dx); wadd(DY, dy);
  wset(MX, clampi(wget(MX) + dx, 0, width - 1)); wset(MY, clampi(wget(MY) + dy, 0, height - 1));
}

void host_input_mouse_at(int x, int y) {
  if (!block) return;
  x = clampi(x, 0, width - 1); y = clampi(y, 0, height - 1);
  wadd(DX, x - wget(MX)); wadd(DY, y - wget(MY));
  wset(MX, x); wset(MY, y);
}

void host_input_wheel(int notches) { wadd(DZ, notches); }

void host_input_text(uint16_t ch) {
  if (!block) return;
  int32_t head = wget(HEAD);
  if ((uint32_t)(head - wget(TAIL)) >= 64) return;
  ((uint16_t*)(block + 296))[head & 63] = ch;
  wset(HEAD, head + 1);
}

void host_input_focus(int focused) {
  wset(FOCUS, focused);
  if (focused || !block) return;
  for (int i = 0; i < 256; i++) host_input_key(i, 0);  // nothing stays held while the game is in the background
  wset(BTN, 0);
}

void host_input_locked(int locked) { wset(LOCKED, locked); }
