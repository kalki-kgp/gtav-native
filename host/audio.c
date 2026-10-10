// Sound output: plays the engine mixer's stereo output from the ring buffer in the engine's heap
// (platform/audio/browser_audio_wasm.cpp; the browser build's audio-worklet.js does the same).
// Ring header (int32 words): write, read, capacity, heartbeat, peak; interleaved float L R at byte 64; 48 kHz.
#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>

#include "gpu_host.h"

static int32_t* hdr;
static float* samples;
static uint32_t mask;

static OSStatus render(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* when, UInt32 bus, UInt32 frames, AudioBufferList* io) {
  float* out = io->mBuffers[0].mData;
  uint32_t read = (uint32_t)__atomic_load_n(&hdr[1], __ATOMIC_SEQ_CST), write = (uint32_t)__atomic_load_n(&hdr[0], __ATOMIC_SEQ_CST);
  uint32_t avail = write - read, take = avail < frames ? avail : frames;
  for (uint32_t i = 0; i < take; i++) {
    uint32_t at = ((read + i) & mask) * 2;
    out[i * 2] = samples[at];
    out[i * 2 + 1] = samples[at + 1];
  }
  for (uint32_t i = take; i < frames; i++) out[i * 2] = out[i * 2 + 1] = 0;  // underrun: silence
  __atomic_store_n(&hdr[1], (int32_t)(read + take), __ATOMIC_SEQ_CST);
  __atomic_fetch_add(&hdr[3], 1, __ATOMIC_SEQ_CST);
  return noErr;
}

void host_audio_start(uint8_t* ring, uint32_t capacity) {
  hdr = (int32_t*)ring;
  samples = (float*)(ring + 64);
  mask = capacity - 1;
  AudioComponentDescription desc = { .componentType = kAudioUnitType_Output, .componentManufacturer = kAudioUnitManufacturer_Apple,
#if TARGET_OS_IPHONE
    .componentSubType = kAudioUnitSubType_RemoteIO,
#else
    .componentSubType = kAudioUnitSubType_DefaultOutput,
#endif
  };
  AudioComponent comp = AudioComponentFindNext(NULL, &desc);
  AudioUnit unit;
  if (!comp || AudioComponentInstanceNew(comp, &unit)) { host_log("audio: no output unit"); return; }
  AudioStreamBasicDescription fmt = { .mSampleRate = 48000, .mFormatID = kAudioFormatLinearPCM, .mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked,
                                      .mBytesPerPacket = 8, .mFramesPerPacket = 1, .mBytesPerFrame = 8, .mChannelsPerFrame = 2, .mBitsPerChannel = 32 };
  AURenderCallbackStruct cb = { render, NULL };
  OSStatus e = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &fmt, sizeof fmt);
  if (!e) e = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
  if (!e) e = AudioUnitInitialize(unit);
  if (!e) e = AudioOutputUnitStart(unit);
  if (e) host_log("audio: could not start output (%d)", (int)e);
  else host_log("audio: playing the engine's mixer ring (%u frames, 48 kHz stereo)", capacity);
}
