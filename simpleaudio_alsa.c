#include "simpleaudio.h"
#include <asoundlib.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>

#define RESAMPLE (1)
#define LATENCY_US (100000) /* 100 ms */

typedef struct { 
  void* audioBuffer;
  snd_pcm_t* handle;
  int samplesLeft; 
  int samplesPlayed;
  int frameSize;
  play_item_t* playListItem;
} alsaAudioBlob_t;

alsaAudioBlob_t* createAudioBlob(void) {
  alsaAudioBlob_t* audioBlob = PyMem_Malloc(sizeof(alsaAudioBlob_t));
  audioBlob->audioBuffer = NULL;
  audioBlob->handle = NULL; 
  return audioBlob;
}

void destroyAudioBlob(alsaAudioBlob_t* audioBlob) {
  void* mutex = audioBlob->playListItem->mutex;
  int lastItemStatus;
  
  PyMem_Free(audioBlob->audioBuffer);
  grabMutex(mutex);
  lastItemStatus = delete_list_item(audioBlob->playListItem);
  releaseMutex(mutex);
  if (lastItemStatus == LAST_ITEM) {
    destroyMutex(mutex);
  }
  PyMem_Free(audioBlob);
}

void* playbackThread(void* threadParam) {
  alsaAudioBlob_t* audioBlob = (alsaAudioBlob_t*)threadParam;
  void* audioPtr;
  int playSamples;
  int result;
  int stop_flag = 0;

  while (audioBlob->samplesLeft > 0 && !stop_flag) {
    grabMutex(audioBlob->playListItem->mutex);
    stop_flag = audioBlob->playListItem->stop_flag;
    releaseMutex(audioBlob->playListItem->mutex);
   
    if (audioBlob->samplesLeft < SIMPLEAUDIO_BUFSZ / audioBlob->frameSize) {
      playSamples = audioBlob->samplesLeft;
    } else {
      playSamples = SIMPLEAUDIO_BUFSZ / audioBlob->frameSize;
    }
    audioPtr = audioBlob->audioBuffer + 
        (size_t)(audioBlob->samplesPlayed * audioBlob->frameSize);
    result = snd_pcm_writei(audioBlob->handle, audioPtr, playSamples);
    if (result < 0) {
      result = snd_pcm_recover(audioBlob->handle, result, 0);
      if (result < 0) {break;} /* unrecoverable error */
    } else {
      audioBlob->samplesPlayed += result;
      audioBlob->samplesLeft -= result;
    }
  }
  
  snd_pcm_drain(audioBlob->handle);
  snd_pcm_close(audioBlob->handle);
  destroyAudioBlob(audioBlob);
  
  pthread_exit(0);
}

simpleaudioError_t playOS(void* audio_data, int len_samples, int num_channels, 
    int bitsPerChan, int sample_rate, play_item_t* play_list_head) {
  alsaAudioBlob_t* audioBlob;
  simpleaudioError_t error = {SIMPLEAUDIO_OK, 0, "", ""};
  int bytes_per_chan = bitsPerChan / 8;
  int bytesPerFrame = bytes_per_chan * num_channels; 
  static char *device = "default";
  snd_pcm_format_t sampleFormat;
  pthread_t playThread;
  int result;
  
  /* set that format appropriately */
  if (bytes_per_chan == 2) {
    sampleFormat = SND_PCM_FORMAT_S16_LE;
  } else {
    error.errorState = SIMPLEAUDIO_ERROR;
    strncpy(error.apiMessage, "Unsupported Sample Format.", SA_ERR_STR_LEN);
    return error;
  }
  
  /* initial allocation and audio buffer copy */
  audioBlob = createAudioBlob();
  audioBlob->audioBuffer = PyMem_Malloc(len_samples * bytesPerFrame);
  memcpy(audioBlob->audioBuffer, audio_data, len_samples * bytesPerFrame);
  audioBlob->samplesLeft = len_samples;
  audioBlob->samplesPlayed = 0;
  audioBlob->frameSize = bytesPerFrame;
  
  /* setup the linked list item for this playback buffer */
  grabMutex(play_list_head->mutex);
  audioBlob->playListItem = new_list_item(play_list_head);
  audioBlob->playListItem->play_id = 0;
  audioBlob->playListItem->stop_flag = 0;
  releaseMutex(play_list_head->mutex);
  
  /* open access to a PCM device (blocking mode)  */
  result = snd_pcm_open(&audioBlob->handle, device, SND_PCM_STREAM_PLAYBACK, 0);
  if (result < 0) {	
    error.errorState = SIMPLEAUDIO_ERROR;
    error.code = (simpleaudioCode_t)result;
    strncpy(error.sysMessage, snd_strerror(result), SA_ERR_STR_LEN);
    strncpy(error.apiMessage, "Error opening PCM device.", SA_ERR_STR_LEN);

    destroyAudioBlob(audioBlob);
    return error;
	}
  /* set the PCM params */
  result = snd_pcm_set_params(audioBlob->handle, sampleFormat, SND_PCM_ACCESS_RW_INTERLEAVED,
    num_channels, sample_rate, RESAMPLE, LATENCY_US);
  if (result < 0) {	
    error.errorState = SIMPLEAUDIO_ERROR;
    error.code = (simpleaudioCode_t)result;
    strncpy(error.sysMessage, snd_strerror(result), SA_ERR_STR_LEN);
    strncpy(error.apiMessage, "Error setting parameters.", SA_ERR_STR_LEN);
    
    snd_pcm_close(audioBlob->handle);
    destroyAudioBlob(audioBlob);
    return error;
	}

  /* fire off the playback thread (and hope the ALSA calls don't fail) */
  result = pthread_create(&playThread, NULL, playbackThread, (void*)audioBlob);
  if (result != 0) {
    error.errorState = SIMPLEAUDIO_ERROR;
    error.code = (simpleaudioCode_t)result;
    strncpy(error.apiMessage, "Could not create playback thread.", SA_ERR_STR_LEN);
    
    snd_pcm_close(audioBlob->handle);
    destroyAudioBlob(audioBlob);
    return error;
  }
  
  return error;
}

