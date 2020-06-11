/*!
 * @file Adafruit_WavePlayer.cpp
 *
 * @mainpage Adafruit WAV file helper Arduino library.
 *
 * @section intro_sec Introduction
 *
 * This is documentation for Adafruit's WavePlayer Arduino library.
 * It's really more a WAV helper than a player, providing some of the
 * lower-level essentials. The examples show how to turn this into a
 * working player.
 *
 * Adafruit invests time and resources providing this open source code,
 * please support Adafruit and open-source hardware by purchasing
 * products from Adafruit!
 *
 * @section dependencies Dependencies
 *
 * This library depends on <a href="https://github.com/adafruit/SdFat">
 * SdFat - Adafruit Fork</a> being present on your system. Please make sure
 * you have installed the latest version before using this library.
 *
 * @section author Author
 *
 * Written by Phil "PaintYourDragon" Burgess for Adafruit Industries.
 *
 * @section license License
 *
 * BSD license, all text here must be included in any redistribution.
 *
 */

#include "Adafruit_WavePlayer.h"

#if defined(__SAMD51__)
#define DAC_BITS 12 ///< Native DAC resolution on SAMD51
#else
#define DAC_BITS 10 ///< Native DAC resolution on SAMD21
#endif
#define SPEAKER_IDLE (1 << (DAC_BITS - 1)) ///< Analog out when not playing

/*!
  @brief  Adafruit_WavePlayer constructor.
  @param  stereoOut   Pass 'true' if output hardware comprises two DACs,
                      (stereo), 'false' for single DAC (mono).
  @param  dacBits     DAC resolution, in bits. 1 to 16, or pass 0 to use
                      hardware-native resolution. Default if unspecified is 0.
  @param  bufferSize  Amount of working space for WAV loading, processing and
                      playback. Actual space may be rounded up slightly.
                      Default if unspecified is 1024 bytes.
  @return Adafruit_WavePlayer object
*/
Adafruit_WavePlayer::Adafruit_WavePlayer(bool stereoOut, uint8_t dacBits,
                                         uint16_t bufferSize)
    : dualDacs(stereoOut) {
  if (dacBits > 16)
    dacBits = 16;
  else if (dacBits == 0)
    dacBits = DAC_BITS;
  dacRes = dacBits;

  // Round bufferSize up to next 8-byte boundary if needed.
  // Stereo 16-bit WAVs are 4 bytes/sample, playback code uses
  // two alternating buffers (loading/playing), hence the 8...
  if (bufferSize < 128)
    bufferSize = 128; // Minimum buffering (2x64 bytes)
  else
    bufferSize = ((bufferSize + 7) / 8) * 8;

  // The two read/convert/playback buffers are allocated as uint8_t arrays.
  // The rounding-up above, plus the behavior of malloc, both ensure these
  // can safely be type-converted and addressed as 16- or 32-bit arrays.
  // A single malloc is used for both; 2nd pointer is assigned to midpoint.
  ab[0].buffer = (uint8_t *)malloc(bufferSize);
  ab[1].buffer = &ab[0].buffer[bufferSize / 2];
}

/*!
  @brief  Adafruit_WavePlayer destructor. Deallocates memory.
*/
Adafruit_WavePlayer::~Adafruit_WavePlayer(void) {
  if (ab[0].buffer)
    free(ab[0].buffer);
}

/*!
  @brief  Table keeps track of differences/similarities between WAV file
          and in-RAM representation.
*/
static const struct {
  uint8_t in;  ///< Number of bytes per sample in WAV file
  uint8_t out; ///< Number of bytes per sample in buffer after processing
  uint8_t max; ///< Greater of 'in' and 'out' values
} bytesPerSample[] = {
    // Table order MUST match wavProcess enums!
    {2, 2, 2}, ///< WAV16MONO   16 bits in, 16 bits out
    {2, 2, 2}, ///< WAV16SPLIT  16 bits in, 16 bits out
    {4, 2, 4}, ///< WAV16MIX    16+16 bits in, 16 bits out
    {4, 4, 4}, ///< WAV16STEREO 16+16 bits in, 16+16 bits out
    {1, 2, 2}, ///< WAV8MONO    8 bits in, 16 bits out
    {1, 2, 2}, ///< WAV8SPLIT   8 bits in, 16 bits out
    {2, 2, 2}, ///< WAV8MIX     8+8 bits in, 16 bits out
    {2, 4, 4}, ///< WAV8STEREO  8+8 bits in, 16+16 bits out
};

/*!
  @brief  Verify WAV file format, read and process initial data, return
          details.
  @param  f           Pointer to File object, ALREADY OPEN for reading.
  @param  sampleRate  Pointer to uint32_t type, where the WAV sample rate
                      will be RETURNED.
  @param  numChannels Pointer to uint16_t type, where the number of audio
                      channels in WAV file (not the playback hardware) will
                      be RETURNED. Can pass NULL (or skip argument) if not
                      used.
  @param  numSamples  Pointer to uint32_t type, where the number of WAV
                      samples initially read will be RETURNED. Can pass
                      NULL (or skip argument) if not used.
  @param  store       Pointer to void* type, where the first batch of
                      DAC-ready processed data was stored (within the
                      object's buffer). Pass NULL or leave off argument
                      if not needed.
  @return One of the wavStatus values:
          WAV_ERR_MALLOC:  Insufficient RAM (during constructor init).
          WAV_ERR_READ:    Can't read from / seek within file.
          WAV_ERR_FORMAT:  Not a WAV file.
          WAV_ERR_VARIANT: WAV type/compression/etc is not supported.
          WAV_LOAD:        Valid WAV, initial data loaded, more follows.
          WAV_EOF:         Valid WAV, all data load, no further data.
*/
wavStatus Adafruit_WavePlayer::start(File &f, uint32_t *sampleRate,
                                     uint16_t *numChannels,
                                     uint32_t *numSamples, void **store) {
  union {
    struct {
      char id[4];
      uint32_t size;
      char data[4];
    } riff; // RIFF chunk
    struct {
      uint16_t compress;
      uint16_t channels;
      uint32_t sampleRate;
      uint32_t bytesPerSecond;
      uint16_t blockAlign;
      uint16_t bitsPerSample;
      uint16_t extraBytes;
    } fmt; // WAV specs
  } buf;

  if (ab[0].buffer == NULL)
    return WAV_ERR_MALLOC;

  file = &f;

  if (sampleRate)
    *sampleRate = 0;
  if (numSamples)
    *numSamples = 0;
  if (numChannels)
    *numChannels = 0;
  if (store)
    *store = NULL;

  if (file->read(&buf, 12) != 12)
    return WAV_ERR_READ;

  // Validate file header
  if (strncmp(buf.riff.id, "RIFF", 4) || strncmp(buf.riff.data, "WAVE", 4))
    return WAV_ERR_FORMAT;

  // Next chunk must be WAV format, chunk size must be 16 or 18.
  if (file->read(&buf, 8) != 8)
    return WAV_ERR_READ;

  uint16_t size;
  if (strncmp(buf.riff.id, "fmt ", 4) ||
      (((size = buf.riff.size) != 16) && (size != 18)))
    return WAV_ERR_VARIANT;

  if (file->read(&buf, size) != size)
    return WAV_ERR_READ;

  // Format must be uncompressed mono or stereo, 8- or 16-bit.
  if (((size == 18) && (buf.fmt.extraBytes > 0)) || (buf.fmt.channels < 1) ||
      (buf.fmt.channels > 2) ||
      ((buf.fmt.bitsPerSample != 8) && (buf.fmt.bitsPerSample != 16))) {
    return WAV_ERR_VARIANT;
  }

  if (buf.fmt.bitsPerSample == 16) { // 16-bit WAV
    if (buf.fmt.channels == 1) {     //   Mono WAV
      if (dualDacs)
        process = WAV16SPLIT; //     Stereo out
      else
        process = WAV16MONO; //     Mono out
    } else {                 //   Stereo WAV
      if (dualDacs)
        process = WAV16STEREO; //     Stereo out
      else
        process = WAV16MIX; //     Mono out
    }
  } else {                       // 8-bit WAV
    if (buf.fmt.channels == 1) { //   Mono WAV
      if (dualDacs)
        process = WAV8SPLIT; //     Stereo out
      else
        process = WAV8MONO; //     Mono out
    } else {                //   Stereo WAV
      if (dualDacs)
        process = WAV8STEREO; //     Stereo out
      else
        process = WAV8MIX; //     Mono out
    }
  }

  // Determine base addresses of processed data -- type-convert to uint16_t*,
  // channel 0/1 data overlaps if file OR DAC is mono, offset by 1 if file
  // AND DACs are BOTH stereo.
  sampleStep = ((process == WAV16STEREO) || (process == WAV8STEREO));
  for (uint8_t i = 0; i < 2; i++) {
    ab[i].processed[0] = (uint16_t *)ab[i].buffer;
    ab[i].processed[1] = &ab[i].processed[0][sampleStep]; // 0 or 1
  }
  sampleStep++; // 1 or 2

  // In most cases, the variable 'cc' (conversion constant) is the number
  // of bits to decimate a 16-bit result to the DAC resolution. But in a
  // couple of "mix" cases, it instead holds the DAC max, or zero.
  if (process == WAV8MIX) {
    cc = (1 << dacRes) - 1; // DAC max
  } else if (process == WAV16MIX) {
    if (dacRes < 16)
      cc = (1 << dacRes) - 1; // DAC max
    else
      cc = 0; // Zero
  } else {
    cc = 16 - dacRes; // Decimation
  }

  if (sampleRate)
    *sampleRate = buf.fmt.sampleRate;
  if (numChannels)
    *numChannels = buf.fmt.channels;

  // Reset counters, indices, etc. and load initial buffer

  wavStatus status;
  uint32_t nSamples;
  chunkBytesToGo = 0;
  chunkPadByte = 0;
  lastRead = 0;
  abIdx = 1;          // read() loads into (1-abIdx)
  ab[1].overflow = 0; // Force rollover on 1st nextSample() call
  sampleIdx = 0;
  status = read(&nSamples, store);
  ab[0].overflow = nSamples * sampleStep;
  if (numSamples)
    *numSamples = nSamples;

  if (status == WAV_OK)
    status = WAV_LOAD;
  return status;
}

/*!
  @brief  Locate and determine size of next data chunk, store in object's
          chunkBytesToGo and chunkPadByte.
  @return One of the wavStatus values:
          WAV_EOF:      No more data chunks; end of file reached.
          WAV_ERR_READ: Seek failed when skipping non-data chunk.
  @note   WAV_EOF has different semantics from start()/read() vs
          nextSample(). In the former case, getting this value means there's
          no more data to READ from the file, but it might continue to PLAY
          from memory; the file can be closed at this point (and any file-
          related data in one's program can be freed). In the latter case,
          getting this value means there is no more data to PLAY from memory;
          it's the last sample and timers can stop on the next interrupt.
*/
wavStatus Adafruit_WavePlayer::nextDataChunk(void) {
  while (chunkBytesToGo <= 0) {
    // Chunks are always word-aligned. If the data size reported in
    // original chunk header was odd, an extra byte must be skipped...
    if (chunkPadByte)
      (void)file->seekCur(1);

    // Read next chunk header. If read is truncated, this could be EOF,
    // or could be legit read error. No way to distinguish really, but
    // EOF is much more likely, give it the benefit of the doubt...
    struct {
      char id[4];
      uint32_t size;
    } header;
    if (file->read(&header, sizeof header) != sizeof header) {
      return WAV_EOF; // Presumed end of file
    }
    if (!strncmp(header.id, "data", 4)) {     // Is it a data chunk?
      chunkBytesToGo = header.size;           // Yes!
      chunkPadByte = header.size & 1;         // Remember if odd size
    } else if (!file->seekCur(header.size)) { // Not data chunk, skip
      return WAV_ERR_READ;                    // Seek failed, weird
    }
  }
  return WAV_OK; // Start of data chunk found!
}

/*!
  @brief  Load first/next buffer-full of data from WAV file, process for
          DAC(s).
  @param  numSamples  Pointer to uint32_t type, the number of samples
                      (not bytes) loaded and processed this pass.
  @param  store       Pointer to void* type, where the DAC-ready processed
                      data was stored (within the object's buffer). Pass
                      NULL or leave off argument if not needed.
  @return One of the wavStatus values:
          WAV_OK:       Data loaded, more follows.
          WAV_EOF:      No more data chunks; end of file reached. This is
                        NOT the marker to end playing, just to end reading
                        (file can now be closed). Playback code should watch
                        for WAV_EOF from nextSample() to stop playing.
          WAV_ERR_READ: Seek failed when skipping non-data chunk.
*/
wavStatus Adafruit_WavePlayer::read(uint32_t *numSamples, void **store) {

  // Number of samples that can be loaded is a function of the buffer size
  // and the larger of the input or output data format (because the output
  // data is written over the input, same buffer). i.e. the data read might
  // not actually fill the whole buffer, because we need the space for
  // expansion in certain cases. buffer[0] and [1] were allocated to always
  // be on 32-bit boundaries, so no alignment worries here.
  int maxSamples = (ab[1].buffer - ab[0].buffer) / bytesPerSample[process].max;
  int maxBytes = maxSamples * bytesPerSample[process].in;

  int samplesRead = 0;               // Tally # of samples loaded
  uint8_t loadIdx = 1 - abIdx;       // Load into not-active buf
  uint8_t *ptr = ab[loadIdx].buffer; // And next load position
  wavStatus status = WAV_OK;
  uint32_t totalBytesRead = 0;

  do {                          // Loop until buffer is full...
    if (chunkBytesToGo <= 0) {  // End of chunk reached?
      status = nextDataChunk(); // Find start of next one
      if ((status != WAV_OK) || (chunkBytesToGo == 0))
        break; // EOF or error
    }

    // How much to read into buffer this pass?
    int bytesToRead = chunkBytesToGo;
    if (bytesToRead > maxBytes)
      bytesToRead = maxBytes;

    int bytesReadThisPass = file->read(ptr, bytesToRead); // What'd we get?
    if (bytesReadThisPass <= 0) {
      status = WAV_ERR_READ; // Read failed
      break;
    }
    maxBytes -= bytesReadThisPass; // Amount of space still in buffer
    ptr += bytesReadThisPass;      // Advance load position in buffer
    totalBytesRead += bytesReadThisPass;
    chunkBytesToGo -= bytesReadThisPass;
  } while (maxBytes > 0); // ...loop until buffer is full
  samplesRead = totalBytesRead / bytesPerSample[process].in;

  // Process what's in the data buffer, even if an error or EOF reached.
  // Calling code should play what it's got and then deal with the status.
  // Processing is done here (rather than in nextSample()) so this has the
  // option of more easily working with DMA (the output buffer can be
  // directly referenced in a DMA descriptor).
  if (samplesRead) {
    // Base address of source & dest are same. Some expansion may occur.
    uint8_t *ptr8in = (uint8_t *)(ab[loadIdx].buffer);
    int16_t *ptr16in = (int16_t *)(ab[loadIdx].buffer);
    uint16_t *ptr16out = (uint16_t *)(ab[loadIdx].buffer);
    int i;

    switch (process) {
    case WAV16MONO:  // 16-bit mono WAV to mono out
    case WAV16SPLIT: // 16-bit mono WAV to stereo out
      // Mono & stereo out use the same conversion from mono source;
      // playback just has left/right base pointers set to same address.
      // Input & output data lengths match: 16 bits in, 16 bits out.
      for (i = 0; i < samplesRead; i++) {
        // Sign-convert 16-bit input, shift down to DAC resolution
        ptr16out[i] = (32768UL + ptr16in[i]) >> cc;
      }
      break;
    case WAV16MIX: // 16-bit stereo WAV to mono out
      // Stereo-to-mono reduction overwrites half the source data;
      // 16 bits output data for every (16+16) bits input.
      if (cc) { // DAC < 16 bits
        for (i = 0; i < samplesRead; i++) {
          // Merge 16+16 bits in (0-131070), scale to full DAC range
          ptr16out[i] =
              ((32768UL + ptr16in[0]) + (32768UL + ptr16in[1])) * cc / 131070;
          ptr16in += 2;
        }
      } else { // 16-bit DAC
        for (i = 0; i < samplesRead; i++) {
          // Merge 16+16 bits in (0-131070), divide by 2 for DAC
          ptr16out[i] = ((32768UL + ptr16in[0]) + (32768UL + ptr16in[1])) >> 1;
          ptr16in += 2;
        }
      }
      break;
    case WAV16STEREO: // 16-bit stereo WAV to stereo out
      for (i = (samplesRead * 2) - 1; i >= 0; i--) { // *2 for stereo conversion
        // Sign-convert 16-bit input, shift down to DAC resolution
        ptr16out[i] = (32768UL + ptr16in[i]) >> cc;
      }
      break;
    case WAV8MONO:  // 8-bit mono WAV to mono out
    case WAV8SPLIT: // 8-bit mono WAV to stereo out
      // Mono & stereo out use the same conversion from mono source;
      // playback just has left/right pointers set to same address.
      // Because data is expanded (8 bits in, 16 bits out), work in
      // reverse so data isn't overwritten as this proceeds.
      for (i = samplesRead - 1; i >= 0; i--) {
        // 8-bit in, 16-bit intermediate, shift down to DAC resolution
        ptr16out[i] = (0x101 * ptr8in[i]) >> cc;
      }
      break;
    case WAV8MIX: // 8-bit stereo WAV to mono out
      // Two 8-bit samples are merged into 16-bit result; + direction OK
      for (i = 0; i < samplesRead; i++) {
        // 8+8 bits in (0-510), scale to full DAC range
        ptr16out[i] = (uint32_t)(ptr8in[0] + ptr8in[1]) * cc / 510;
        ptr8in += 2;
      }
      break;
    case WAV8STEREO: // 8-bit stereo WAV to stereo out
      // Because data is expanded (16 bits in, 16+16 bits out), work in
      // reverse so data isn't overwritten as this proceeds.
      for (i = (samplesRead * 2) - 1; i >= 0; i--) { // *2 for stereo conversion
        // 8-bit in, 16-bit intermediate, shift down to DAC resolution
        ptr16out[i] = (0x101 * ptr8in[i]) >> cc;
      }
      break;
    }
  }

  nextBufReady = 1;
  lastRead = (status != WAV_OK);
  ab[loadIdx].overflow = samplesRead * sampleStep;
  if (numSamples)
    *numSamples = samplesRead;
  if (store)
    *store = (void *)ab[loadIdx].buffer;

  return status;
}

/*!
  @brief  Fetch next WAV sample for DAC(s).
  @param  result  Pointer to wavSample type, where DAC-ready data will
                  be placed..
  @return One of the wavStatus values:
          WAV_OK:    Item returned, more available.
          WAV_EOF:   No more data; stop playing (result untouched).
          WAV_LOAD:  Item returned, call read() asap.
          WAV_STALL: Data not ready; read() operation hasn't completed,
                     contents of result will be left untouched.
  @note   WAV_EOF has different semantics from start()/read() vs
          nextSample(). In the former case, getting this value means there's
          no more data to READ from the file, but it might continue to PLAY
          from memory; the file can be closed at this point (and any file-
          related data in one's program can be freed). In the latter case,
          getting this value means there is no more data to PLAY from memory;
          it's the last sample and timers can stop on the next interrupt.
*/
wavStatus Adafruit_WavePlayer::nextSample(wavSample *result) {
  wavStatus status = WAV_OK;
  if (sampleIdx >= ab[abIdx].overflow) { // Past end of active buffer?
    if (lastRead)
      return WAV_EOF;    // No more data? Return EOF.
    if (nextBufReady) {  // Next buffer loaded?
      abIdx = 1 - abIdx; // Switch A/B buffers
      sampleIdx = 0;     // Reset to beginning
      nextBufReady = 0;  // Reset next-buffer flag
      status = WAV_LOAD; // Load more pls!
    } else {
      return WAV_ERR_STALL; // Next buffer read() not finished
    }
  }
  result->channel0 = ab[abIdx].processed[0][sampleIdx]; // may point to
  result->channel1 = ab[abIdx].processed[1][sampleIdx]; // same data, or not
  sampleIdx += sampleStep;                              // 1 or 2

  return status;
}

/*!
  @brief  Switch active buffer index.
  @note   Buffer swapping is done automatically by the nextSample() function,
          but DMA-driven code (which doesn't call nextSample()) will need to
          invoke its own buffer swaps.
*/
void Adafruit_WavePlayer::swapBuffers(void) { abIdx = 1 - abIdx; }

/*!
  @brief  A self-contained easy WAV player that does not use interrupts or
          DMA. This is a BLOCKING function and does not return until the WAV
          ends. If non-blocking (background) playing is required, other
          functions in this library must be used along with additional code,
          see examples (this is so the library need not be tied to specific
          timer or DMA peripherals). THIS FUNCTION CHANGES THE analogWrite()
          RESOLUTION USING analogWriteResolution() AND DOES NOT RESTORE IT.
          If you need a specific analogWrite() resolution in your own code,
          you'll need to set that up after using this function. If audio
          hardware requires a SPEAKER-ENABLE PIN, that also must be set up
          in your own code BEFORE calling this function. Also, playback may
          stutter slightly, especially with high sample rate WAVs, it's just
          an unavoidable aspect of this function not using timers or DMA.
  @param  f         Pointer to File object, ALREADY OPEN for reading.
  @param  leftOut   Arduino pin number for mono analog audio out
                    OR left channel if stereo out is available.
  @param  rightOut  Arduino pin number for right channel if stereo out is
                    available. For mono out hardware, leave this argument
                    off, or pass the same value as leftOut, or -1.
  @return One of the wavStatus values:
          WAV_OK:          File finished playing successfully.
          WAV_ERR_MALLOC:  Insufficient RAM (during constructor init).
          WAV_ERR_READ:    Can't read from / seek within file.
          WAV_ERR_FORMAT:  Not a WAV file.
          WAV_ERR_VARIANT: WAV type/compression/etc is not supported.
*/
wavStatus Adafruit_WavePlayer::simplePlayer(File &f, int8_t leftOut,
                                            int8_t rightOut) {
  if (rightOut < 0)
    rightOut = leftOut;
  if (leftOut < 0)
    leftOut = rightOut; // Shouldn't happen
  if (leftOut < 0)
    return WAV_OK; // No output pins, assume "done."

  uint32_t sampleRate, r2;
  uint32_t t, startTime;
  wavSample sample;
  wavStatus status = start(f, &sampleRate);
  if ((status == WAV_LOAD) || (status == WAV_EOF)) {
    analogWriteResolution(DAC_BITS); // See notes above
    r2 = sampleRate / 2;
    startTime = micros() - 1000UL; // Force 1st sample to play immediately
    for (uint32_t sampleNum = 0;; sampleNum++) {
      status = nextSample(&sample);
      while (((micros() - startTime + 50) / 100) <
             ((sampleNum * 10000UL + r2) / sampleRate))
        ;
      if (status == WAV_EOF)
        break;
      analogWrite(leftOut, sample.channel0);
      if (rightOut != leftOut)
        analogWrite(rightOut, sample.channel1);
      if (status == WAV_LOAD) { // Time to load more data?
        yield();
        if ((status = read()) == WAV_ERR_READ)
          break;
      }
    }
    analogWrite(leftOut, SPEAKER_IDLE);
    if (rightOut != leftOut)
      analogWrite(rightOut, SPEAKER_IDLE);
    return WAV_OK;
  } else {
    return status;
  }
}
