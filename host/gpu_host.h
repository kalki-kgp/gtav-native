// What the renderer (C++) needs from the rest of the host (C).
#ifndef GTA_GPU_HOST_H
#define GTA_GPU_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Everything the platform front end decides before the engine starts.
typedef struct {
  const char* data_dir;    // the mirrored /data folder (manifest.json, common/, x64/)
  const char* user_dir;    // writable folder backing the engine's own paths (settings, savegames)
  const char* shader_dir;  // translated shaders; NULL: <data_dir>/../b/<build>/shaders
  const char* gpu_mode;    // "dawn", "null" (consume the commands, draw nothing) or "off" (engine without a renderer)
  int window;              // there is a surface to present into
  int width, height;       // back buffer
  int argc;                // extra engine arguments
  char** argv;
} host_config;
// Starts the engine on its own threads and returns.
void host_start(const host_config* config);

// Diagnostics: the call stack of every thread, into the log.
void host_dump_threads(void);

uint8_t* host_mem_base(void);
void host_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
double host_now_ms(void);
void host_log_memory(void);
uint32_t host_futex_wait32(uint64_t addr, uint32_t expected, int64_t timeout_ns);
uint32_t host_futex_notify(uint64_t addr, uint32_t count);
const char* host_shader_dir(void);
const char* host_user_dir(void);  // the writable folder

// The CAMetalLayer of the game window; NULL when the host runs without one. Blocks until the window exists.
void* host_window_layer(void);
// Runs the platform's event loop on the calling (main) thread; ends the process when the window is closed.
void host_window_run(int width, int height);
// Before the engine starts: whether there will be a window at all.
void host_window_want(int wanted);
// iOS: the app hands over the CAMetalLayer of its game view.
void host_window_set_layer(void* layer);

// ---- input (host/input.c): the platform front end reports events, they land in the engine's input block --------------------
// The engine published its input block. startMode: 0 none, 1 Story Mode, 2 Sandbox Mode on the GTA V map, 3 Sandbox Mode on
// env_test (the page's start menu). width/height: the back buffer, which mouse coordinates are in.
void host_input_attach(uint8_t* block, int width, int height, int startMode);
int host_input_width(void);
int host_input_height(void);
void host_input_key(int vk, int down);          // Windows virtual-key code
void host_input_button(int bit, int down);      // 0 left, 1 right, 2 middle
void host_input_mouse_delta(int dx, int dy);    // relative motion (mouse captured, or a touch look pad)
void host_input_mouse_at(int x, int y);         // absolute, in back-buffer pixels
void host_input_wheel(int notches);
void host_input_text(uint16_t ch);
void host_input_focus(int focused);             // losing focus releases every key and button
void host_input_locked(int locked);             // relative-motion mode (the page's pointer lock)

// Starts playing the engine's audio ring (interleaved float stereo, 48 kHz).
void host_audio_start(uint8_t* ring, uint32_t capacity);

void gpu_start(uint64_t ring, uint64_t size, uint64_t dbg);
void gpu_suspend(int on);  // 1 while the app is in the background: no frames are presented
void gpu_null_start(uint64_t ring, uint64_t size, uint64_t dbg);

#ifdef __cplusplus
}
#endif

#endif
