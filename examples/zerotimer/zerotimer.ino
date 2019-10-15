// Adafruit_WavePlayer example using Adafruit_ZeroTimer
// (within Adafruit_Arcada) for timing control.

#include <Adafruit_Arcada.h>
#include <Adafruit_WavePlayer.h>

// EVEN FOR 8-BIT WAVS, it's important to use the NATIVE DAC RESOLUTION:
// 10 bits on SAMD21, 12 bits on SAMD51. Initialize DAC by calling
// analogWriteResolution(), passing that value. DAC upscaling in Arduino
// is handled improperly and will result in a slight DC offset. Using
// the native DAC res (upsampling in Adafruit_WavePlayer) corrects this.
#if defined(__SAMD51__)
  #define DAC_BITS   12
#else
  #define DAC_BITS   10
#endif
#define SPEAKER_IDLE (1 << (DAC_BITS - 1))
#if defined(ARCADA_LEFT_AUDIO_PIN)
  #define STEREO_OUT true
#else
  #define STEREO_OUT false
#endif
#define BUFFER_SIZE  2048  // Two 1K load buffers

Adafruit_Arcada     arcada;
Adafruit_WavePlayer player(STEREO_OUT, DAC_BITS, BUFFER_SIZE);
bool                readflag = false; // See wavOutCallback()
bool                playing  = false;

const char *wav_path = "wavs";
struct wavlist { // Linked list of WAV filenames
  char           *filename;
  struct wavlist *next;
} *wavListStart = NULL, *wavListPtr = NULL;
#define MAX_WAV_FILES 20

// Crude error handler. Prints message to Serial Monitor, blinks LED.
void fatal(const char *message, uint16_t blinkDelay) {
  Serial.begin(9600);
  Serial.println(message);
  for(bool ledState = HIGH;; ledState = !ledState) {
    digitalWrite(LED_BUILTIN, ledState);
    delay(blinkDelay);
  }
}

void setup(void) {
  if(!arcada.arcadaBegin())     fatal("Arcada init fail!", 100);
  // TinyUSB teporarily disabled -- was leading to filesys corruption?
#if 0 && defined(USE_TINYUSB)
  if(!arcada.filesysBeginMSD()) fatal("No filesystem found!", 250);
#else
  if(!arcada.filesysBegin())    fatal("No filesystem found!", 250);
#endif
  Serial.begin(9600);
  while(!Serial) yield();

  analogWriteResolution(DAC_BITS); // See notes above

  // Skim folder for all WAVs, make linked list
  File            entry;
  struct wavlist *wptr;
  char            filename[SD_MAX_FILENAME_SIZE+1];
  // Scan wav_path for .wav files:
  for(int i=0; i<MAX_WAV_FILES; i++) {
    yield();
    entry = arcada.openFileByIndex(wav_path, i, O_READ, "wav");
    if(!entry) break;
    // Found one, alloc new wavlist struct, try duplicating filename
    if((wptr = (struct wavlist *)malloc(sizeof(struct wavlist)))) {
      entry.getName(filename, SD_MAX_FILENAME_SIZE);
      if((wptr->filename = strdup(filename))) {
        // Alloc'd OK, add to linked list...
        if(wavListPtr) {           // List already started?
          wavListPtr->next = wptr; // Point prior last item to new one
        } else {
          wavListStart = wptr;     // Point list head to new item
        }
        wavListPtr = wptr;         // Update last item to new one
      } else {
        free(wptr);                // Alloc failed, delete interim stuff
      }
    }
    entry.close();
  }
  if(wavListPtr) {                   // Any items in WAV list?
    wavListPtr->next = wavListStart; // Point last item's next to list head (list is looped)
    wavListPtr       = wavListStart; // Update list pointer to head
    arcada.chdir(wav_path);
  }
}

void loop(void) {
  File file;
Serial.printf("Trying: '%s'\n", wavListPtr->filename);
  if(file = arcada.open(wavListPtr->filename, FILE_READ)) {
    uint32_t sampleRate;
    do { // Wait for prior WAV (if any) to finish playing
      yield();
    } while(playing);
    wavStatus status = player.start(file, &sampleRate);
    if((status == WAV_LOAD) || (status == WAV_EOF)) {
      // Begin audio playback
      playing = true;
      arcada.enableSpeaker(true);
      arcada.timerCallback(sampleRate, wavOutCallback);
      do { // Repeat this loop until WAV_EOF or WAV_ERR_*
        if(readflag) {
          yield();
          readflag = false; // reset flag BEFORE the read!
          status   = player.read();
        }
      } while((status == WAV_OK) || (status == WAV_LOAD));
      // Might be EOF, might be error
      Serial.print("WAV end: ");
      Serial.println(status);
    } else {
      Serial.print("WAV error: ");
      Serial.println(status);
    }
    file.close();
  }

  wavListPtr = wavListPtr->next; // Will loop around from end to start of list

  // Audio might be continuing to play at this point! It's switched
  // off in wavOutCallback() below only when final buffer is depleted.
}

// Single-sample-playing callback function for timerCallback() above.
void wavOutCallback(void) {
  wavSample sample;
  wavStatus status = player.nextSample(&sample);
  if((status == WAV_OK) || (status == WAV_LOAD)) {
#if STEREO_OUT
    analogWrite(ARCADA_LEFT_AUDIO_PIN , sample.channel0);
    analogWrite(ARCADA_RIGHT_AUDIO_PIN, sample.channel1);
#else
    analogWrite(ARCADA_AUDIO_OUT      , sample.channel0);
#endif
    // If nextSample() indicates it's time to read more WAV data,
    // set a flag and handle it in loop(), not here in the interrupt!
    // The read operation will almost certainly take longer than a
    // single audio sample cycle and would cause audio to stutter.
    if(status == WAV_LOAD) readflag = true;
  } else if(status == WAV_EOF) {
    // End of WAV file reached, stop timer, stop audio
    arcada.timerStop();
#if STEREO_OUT
    analogWrite(ARCADA_LEFT_AUDIO_PIN , SPEAKER_IDLE);
    analogWrite(ARCADA_RIGHT_AUDIO_PIN, SPEAKER_IDLE);
#else
    analogWrite(ARCADA_AUDIO_OUT      , SPEAKER_IDLE);
#endif
    arcada.enableSpeaker(false);
    playing = false;
  } // else WAV_ERR_STALL, do nothing
}
