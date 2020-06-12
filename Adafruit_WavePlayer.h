/*!
 * @file Adafruit_WavePlayer.h
 *
 * This is part of Adafruit's WavePlayer Arduino library.
 * It's really more a WAV helper than a player, providing some of the
 * lower-level essentials. The examples show how to turn this into a
 * working player.
 *
 * Written by Phil "PaintYourDragon" Burgess for Adafruit Industries.
 *
 * BSD license, all text here must be included in any redistribution.
 *
 */

#if !defined(_ADAFRUIT_WAVEPLAYER_H_)
#define _ADAFRUIT_WAVEPLAYER_H_

#include <Arduino.h>
#include <SdFat.h>

/*!
  @brief  Return codes used by functions in this library
*/
typedef enum {
  WAV_OK,          ///< Successful operation; non-error
  WAV_LOAD,        ///< Load more data; buffer is available
  WAV_EOF,         ///< End of file reached (valid status, not error)
  WAV_ERR_READ,    ///< File read error
  WAV_ERR_FORMAT,  ///< File is not WAV format
  WAV_ERR_VARIANT, ///< Unsupported WAV variant
  WAV_ERR_MALLOC,  ///< Allocation failed
  WAV_ERR_STALL,   ///< Attempting to play from buffer before fully loaded
  WAV_ERR_NOFILE,  ///< No WAV file/buffer is currently loaded
} wavStatus;

/*!
  @brief  Single sample from WAV file in DAC-ready format
*/
typedef struct {
  unsigned channel0 : 16; ///< Ostensibly left channel
  unsigned channel1 : 16; ///< Ostensibly right channel
} wavSample;

/*!
  @brief  WAV processing mode, used internally within library
*/
typedef enum {
  WAV16MONO,   ///< 16-bit mono WAV to mono DAC
  WAV16SPLIT,  ///< 16-bit mono WAV to stereo DACs
  WAV16MIX,    ///< 16-bit stereo WAV to mono DAC
  WAV16STEREO, ///< 16-bit stereo WAV to stereo DACs
  WAV8MONO,    ///< 8-bit mono WAV to mono DAC
  WAV8SPLIT,   ///< 8-bit mono WAV to stereo DACs
  WAV8MIX,     ///< 8-bit stereo WAV to mono DAC
  WAV8STEREO   ///< 8-bit stereo WAV to stereo DACs
} wavProcess;

/*!
  @brief  WAV helper class. NOT a fully self-contained player,
          just the file parsing and data conversion parts.
*/
class Adafruit_WavePlayer {
public:
  Adafruit_WavePlayer(bool stereoOut, uint8_t dacBits = 0,
                      uint16_t bufferSize = 1024);
  ~Adafruit_WavePlayer(void);
  wavStatus start(File &f, uint32_t *sampleRate, uint16_t *numChannels = NULL,
                  uint32_t *numSamples = NULL, void **store = NULL);
  wavStatus read(uint32_t *numSamples = NULL, void **store = NULL);
  wavStatus nextSample(wavSample *result);
  void swapBuffers(void);
  wavStatus simplePlayer(File &f, int8_t leftPin, int8_t rightPin = -1);

private:
  wavStatus nextDataChunk(void);
  File *file; ///< Currently-open WAV File
  struct {
    uint8_t *buffer;        ///< Load/convert/output working space
    uint16_t *processed[2]; ///< Channel 0/1 pointer to DAC-ready data
    uint32_t overflow;      ///< Amt where sampleIdx rolls over
  } ab[2];                  ///< A/B buffers
  int32_t chunkBytesToGo;   ///< As-yet-unread bytes in data chunk
  uint32_t sampleIdx;
  wavProcess process;        ///< WAV-to-DAC conversion method
  uint16_t cc;               ///< Conversion constant for 'process'
  unsigned dacRes : 5;       ///< DAC resolution, in bits (16 bits max)
  unsigned dualDacs : 1;     ///< 1 if dual DACs, 0 if single DAC
  unsigned chunkPadByte : 1; ///< 1 if odd number of bytes in chunk
  unsigned abIdx : 1;        ///< Currently-playing ab[]
  unsigned sampleStep : 2;   ///< Sample increment in processed[] array
  unsigned lastRead : 1;     ///< 1 if last read() call returned EOF
  unsigned nextBufReady : 1; ///< 1 if ready for A/B buffer swap
};

#endif // _ADAFRUIT_WAVEPLAYER_H_
