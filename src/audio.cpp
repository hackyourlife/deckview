#include <cstdlib>
#include <cstring>
#include <pthread.h>
#include <pulse/simple.h>

#include "deckview.h"

static pa_simple* pulse;
static pthread_t thread;
static pthread_mutex_t mutex;

static volatile bool quit;

#define	AUDIO_BUFCNT	4

static volatile void* audio_data[AUDIO_BUFCNT];
static volatile size_t audio_size[AUDIO_BUFCNT];
static volatile unsigned int audio_buf_r;
static volatile unsigned int audio_buf_w;

bool AXInit(unsigned int channels, unsigned int bit)
{
	pa_sample_spec ss;

	pthread_mutex_init(&mutex, NULL);

	switch(bit) {
		case 16:
			ss.format = PA_SAMPLE_S16NE;
			break;
		case 24:
			ss.format = PA_SAMPLE_S24NE;
			break;
	}

	ss.channels = channels;
	ss.rate = 48000;

	pulse = pa_simple_new(NULL,	// Use the default server.
		"DeckLink View",	// Our application's name.
		PA_STREAM_PLAYBACK,
		NULL,			// Use the default device.
		"Capture Audio",	// Description of our stream.
		&ss,			// Our sample format.
		NULL,			// Use default channel map
		NULL,			// Use default buffering attributes.
		NULL);			// Ignore error code.

	return pulse != NULL;
}

static void* ax_thread(void* arg)
{
	void* buf = NULL;
	size_t sz = 0;
	while(!quit) {
		pthread_mutex_lock(&mutex);
		unsigned int r = audio_buf_r;
		audio_buf_r = (audio_buf_r + 1) % AUDIO_BUFCNT;

		volatile void* abuf = audio_data[r];
		size_t asz = audio_size[r];

		if(asz == 0) {
			pthread_mutex_unlock(&mutex);
			continue;
		}

		if(buf == NULL) {
			buf = malloc(asz);
			sz = asz;
		} else if(sz != asz) {
			buf = realloc((void*) buf, asz);
			sz = asz;
		}

		memcpy(buf, (void*) abuf, asz);
		pthread_mutex_unlock(&mutex);

		if(buf && sz > 0) {
			pa_simple_write(pulse, buf, sz, NULL);
		}
	}

	if(buf) {
		free(buf);
	}

	return NULL;
}

void AXStart(void)
{
	quit = false;
	audio_buf_r = AUDIO_BUFCNT - 1;
	audio_buf_w = 0;
	pthread_create(&thread, NULL, ax_thread, NULL);
}

void AXStop(void)
{
	if(thread) {
		quit = true;
		pthread_join(thread, NULL);
	}
	thread = 0;
}

void AXDestroy(void)
{
	if(thread) {
		AXStop();
	}
	pa_simple_free(pulse);
}

void AXPlay(void* data, size_t size)
{
	pthread_mutex_lock(&mutex);
	unsigned int w = audio_buf_w;
	audio_buf_w = (audio_buf_w + 1) % AUDIO_BUFCNT;

	volatile void** abuf = &audio_data[w];
	volatile size_t* asz = &audio_size[w];

	if(*abuf == NULL) {
		*abuf = malloc(size);
		*asz = size;
	} else if(size != *asz) {
		*abuf = realloc((void*) *abuf, size);
		*asz = size;
	}
	memcpy((void*) *abuf, data, size);
	pthread_mutex_unlock(&mutex);
}
