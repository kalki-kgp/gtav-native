// iOS: the app owns the UI (ios/app); the renderer only needs the CAMetalLayer of its game view.
#include <pthread.h>

#include "gpu_host.h"

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static void* layer;

void host_window_set_layer(void* l) {
  pthread_mutex_lock(&mu);
  layer = l;
  pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);
}

void* host_window_layer(void) {
  pthread_mutex_lock(&mu);
  while (!layer) pthread_cond_wait(&cv, &mu);
  void* l = layer;
  pthread_mutex_unlock(&mu);
  return l;
}

void host_window_want(int wanted) {}
void host_window_run(int width, int height) {}
