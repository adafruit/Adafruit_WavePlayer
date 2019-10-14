// TO DO: set this up from the other example once that's all working,
// then make DMA-specific changes here.

#include <Adafruit_WavePlayer.h>

#if defined(__SAMD51__)
  #define DAC_BITS 12
#else
  #define DAC_BITS 10
#endif
#define SPEAKER_IDLE (1 << (DAC_BITS - 1))
#define STEREO_OUT   true
#define BUFFER_SIZE  1024

Adafruit_WavePlayer player(STEREO_OUT, DAC_BITS, BUFFER_SIZE);

void setup(void) {
  analogWriteResolution(DAC_BITS);
}

void loop(void) {
}
