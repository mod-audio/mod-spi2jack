/*
 * This file is part of mod-spi2jack.
 *
 * mod-spi2jack is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * mod-spi2jack is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with mod-spi2jack.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alsa/asoundlib.h>

#include <jack/jack.h>
#include <jack/metadata.h>
#include <jack/uuid.h>

#include <pthread.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "mod-semaphore.h"

#define ALSA_SOUNDCARD_DEFAULT_ID "DUOX"
#define ALSA_CONTROL_HP_CV_MODE   "Headphone/CV Mode"

/*
 * TODO:
 *  1. device number from args
 */

// custom jack flag used for cv
// needed because we prefer jack2 which doesn't always have working metadata
#define JackPortIsControlVoltage 0x100

#define MAX_RAW_IIO_VALUE   4095
#define MAX_RAW_IIO_VALUE_f 4095.0f

typedef struct {
  jack_client_t* client;
  jack_port_t* port1;
  jack_port_t* port2;
  float value1, value2;
  FILE *out1f, *out2f;
  float* tmpSortArray;
  volatile bool run;
  volatile bool cvEnabled;
  pthread_t thread;
  sem_t sem;
  // for knowing wherever the cv/hp mode is enabled or not
  snd_mixer_t* mixer;
  snd_mixer_elem_t* mixerElem;
} jack2spi_t;

static bool _get_alsa_switch_value(snd_mixer_elem_t* const elem)
{
    int val = 0;
    snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_MONO, &val);
    return (val != 0);
}

static void* write_spi_thread(void* ptr)
{
    jack2spi_t* const jack2spi = (jack2spi_t*)ptr;

    FILE* const out1f = jack2spi->out1f;
    FILE* const out2f = jack2spi->out2f;

    char buf[12];
    float value1, value2;
    uint16_t rvalue1, rvalue2;

    while (jack2spi->run)
    {
        // handle mixer changes
        if (jack2spi->mixer != NULL)
        {
            snd_mixer_handle_events(jack2spi->mixer);
            jack2spi->cvEnabled = _get_alsa_switch_value(jack2spi->mixerElem);
        }

        if (sem_timedwait_secs(&jack2spi->sem, 1) != 0)
            continue;

        // read the values as soon as we get unlocked
        value1 = jack2spi->value1;
        value2 = jack2spi->value2;

        if (jack2spi->cvEnabled)
        {
            // out1
            if (value1 <= 0.0f)
                rvalue1 = 0;
            else if (value1 >= 10.0f)
                rvalue1 = MAX_RAW_IIO_VALUE;
            else
                rvalue1 = (uint16_t)(int)(value1 / 10.0f * MAX_RAW_IIO_VALUE_f + 0.5f);

            // out2
            if (value2 <= 0.0f)
                rvalue2 = 0;
            else if (value2 >= 10.0f)
                rvalue2 = MAX_RAW_IIO_VALUE;
            else
                rvalue2 = (uint16_t)(int)(value2 / 10.0f * MAX_RAW_IIO_VALUE_f + 0.5f);
        }
        else
        {
            rvalue1 = rvalue2 = 0;
        }

        if (snprintf(buf, sizeof(buf), "%u\n", rvalue1) >= (int)sizeof(buf)-1)
        {
            buf[sizeof(buf)-2] = '\n';
            buf[sizeof(buf)-1] = '\0';
        }
        else
        {
            buf[sizeof(buf)-1] = '\0';
        }

        rewind(out1f);
        fwrite(buf, strlen(buf)+1, 1, out1f);

        if (snprintf(buf, sizeof(buf), "%u\n", rvalue2) >= (int)sizeof(buf)-1)
        {
            buf[sizeof(buf)-2] = '\n';
            buf[sizeof(buf)-1] = '\0';
        }
        else
        {
            buf[sizeof(buf)-1] = '\0';
        }

        rewind(out2f);
        fwrite(buf, strlen(buf)+1, 1, out2f);
    }

    return NULL;
}

static inline float get_median_value(float* tmparray, const float* source, jack_nframes_t nframes)
{
    float temp;

    memcpy(tmparray, source, sizeof(float)*nframes);

    for (jack_nframes_t i=0; i < nframes ; i++)
    {
        for (jack_nframes_t j=0; j < nframes - 1; j++)
        {
            if (tmparray[j] > tmparray[j+1])
            {
                temp          = tmparray[j];
                tmparray[j]   = tmparray[j+1];
                tmparray[j+1] = temp;
            }
        }
    }

    return (tmparray[nframes-1] + tmparray[nframes/2])/2.0f;
}

static int bufsize_callback(jack_nframes_t nframes, void* arg)
{
    jack2spi_t* const jack2spi = (jack2spi_t*)arg;

    free(jack2spi->tmpSortArray);
    jack2spi->tmpSortArray = malloc(sizeof(float)*nframes);

    return 0;
}

static int process_callback(jack_nframes_t nframes, void* arg)
{
    jack2spi_t* const jack2spi = (jack2spi_t*)arg;

    if (jack2spi->cvEnabled)
    {
        const float* const port1buf = jack_port_get_buffer(jack2spi->port1, nframes);
        const float* const port2buf = jack_port_get_buffer(jack2spi->port2, nframes);

        jack2spi->value1 = get_median_value(jack2spi->tmpSortArray, port1buf, nframes);
        jack2spi->value2 = get_median_value(jack2spi->tmpSortArray, port2buf, nframes);
    }
    else
    {
        jack2spi->value1 = jack2spi->value2 = 0.0f;
    }

    sem_post(&jack2spi->sem);

    return 0;
}

JACK_LIB_EXPORT
int jack_initialize(jack_client_t* client, const char* load_init);

JACK_LIB_EXPORT
void jack_finish(void* arg);

JACK_LIB_EXPORT
int jack_initialize(jack_client_t* client, const char* load_init)
{
    FILE* const fname = fopen("/sys/bus/iio/devices/iio:device1/name", "rb");

    if (!fname)
    {
      fprintf(stderr, "Cannot get iio device\n");
      return EXIT_FAILURE;
    }

    char namebuf[32];
    memset(namebuf, 0, sizeof(namebuf));
    if (fread(namebuf, sizeof(namebuf), 1, fname) == 0 && feof(fname) == 0)
    {
        fprintf(stderr, "Cannot read iio device name\n");
        return EXIT_FAILURE;
    }

    namebuf[sizeof(namebuf)-1] = '\0';
    namebuf[strlen(namebuf)-1] = '\0';
    fprintf(stdout, "Opening iio device '%s'...\n", namebuf);

    fclose(fname);

    FILE* const out1f = fopen("/sys/bus/iio/devices/iio:device1/out_voltage0_raw", "wb");
    if (!out1f)
    {
        fprintf(stderr, "Cannot get iio raw input 1 file\n");
        return EXIT_FAILURE;
    }

    FILE* const out2f = fopen("/sys/bus/iio/devices/iio:device1/out_voltage1_raw", "wb");
    if (!out2f)
    {
        fprintf(stderr, "Cannot get iio raw input 1 file\n");
        fclose(out2f);
        return EXIT_FAILURE;
    }

    jack2spi_t* const jack2spi = calloc(1, sizeof(jack2spi_t));
    if (!jack2spi)
    {
        fprintf(stderr, "Out of memory\n");
        return EXIT_FAILURE;
    }

    jack2spi->out1f = out1f;
    jack2spi->out2f = out2f;
    jack2spi->run = true;

    sem_init(&jack2spi->sem, 0, 0);

    // setup alsa-mixer listener
    if (snd_mixer_open(&jack2spi->mixer, SND_MIXER_ELEM_SIMPLE) == 0)
    {
        snd_mixer_selem_id_t* sid;

        char soundcard[32] = "hw:";

        const char* const cardname = getenv("MOD_SOUNDCARD");
        if (cardname != NULL)
            strncat(soundcard, cardname, 28);
        else
            strncat(soundcard, ALSA_SOUNDCARD_DEFAULT_ID, 28);

        soundcard[31] = '\0';

        if (snd_mixer_attach(jack2spi->mixer, soundcard) == 0 &&
            snd_mixer_selem_register(jack2spi->mixer, NULL, NULL) == 0 &&
            snd_mixer_load(jack2spi->mixer) == 0 &&
            snd_mixer_selem_id_malloc(&sid) == 0)
        {
            snd_mixer_selem_id_set_index(sid, 0);
            snd_mixer_selem_id_set_name(sid, ALSA_CONTROL_HP_CV_MODE);

            jack2spi->mixerElem = snd_mixer_find_selem(jack2spi->mixer, sid);

            if (jack2spi->mixerElem != NULL)
                jack2spi->cvEnabled = _get_alsa_switch_value(jack2spi->mixerElem);

            snd_mixer_selem_id_free(sid);
        }

        if (jack2spi->mixerElem == NULL)
        {
            snd_mixer_close(jack2spi->mixer);
            jack2spi->mixer = NULL;
        }
    }

    // setup writing thread
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_JOINABLE);
    pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setscope(&attributes, (client != NULL) ? PTHREAD_SCOPE_PROCESS : PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setschedpolicy(&attributes, SCHED_FIFO);

    struct sched_param rt_param;
    memset(&rt_param, 0, sizeof(rt_param));
    rt_param.sched_priority = 78;

    pthread_attr_setschedparam(&attributes, &rt_param);

    pthread_create(&jack2spi->thread, &attributes, write_spi_thread, (void*)jack2spi);
    pthread_attr_destroy(&attributes);

    jack2spi->client = client;

    // Register ports.
    const long unsigned port_flags = JackPortIsTerminal|JackPortIsPhysical|JackPortIsInput|JackPortIsControlVoltage;
    jack2spi->port1 = jack_port_register(client, "playback_1", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    jack2spi->port2 = jack_port_register(client, "playback_2", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);

    if (!jack2spi->port1 || !jack2spi->port2) {
        fprintf(stderr, "Can't register jack ports\n");
        fclose(out1f);
        fclose(out2f);
        free(jack2spi);
        return EXIT_FAILURE;
    }

    // Set port aliases and metadata
    jack_port_set_alias(jack2spi->port1, "CV Playback 1");
    jack_port_set_alias(jack2spi->port2, "CV Playback 2");

    const jack_uuid_t uuid1 = jack_port_uuid(jack2spi->port1);
    const jack_uuid_t uuid2 = jack_port_uuid(jack2spi->port2);

    if (!jack_uuid_empty(uuid1))
    {
        jack_set_property(client, uuid1, JACK_METADATA_PRETTY_NAME, "CV Playback 1", "text/plain");
        jack_set_property(client, uuid1, JACK_METADATA_SIGNAL_TYPE, "CV", "text/plain");
        jack_set_property(client, uuid1, JACK_METADATA_ORDER, "1", NULL);
        jack_set_property(client, uuid1, "http://lv2plug.in/ns/lv2core#minimum", "0", NULL);
        jack_set_property(client, uuid1, "http://lv2plug.in/ns/lv2core#maximum", "10", NULL);
    }

    if (!jack_uuid_empty(uuid2))
    {
        jack_set_property(client, uuid2, JACK_METADATA_PRETTY_NAME, "CV Playback 2", "text/plain");
        jack_set_property(client, uuid2, JACK_METADATA_SIGNAL_TYPE, "CV", "text/plain");
        jack_set_property(client, uuid2, JACK_METADATA_ORDER, "2", NULL);
        jack_set_property(client, uuid2, "http://lv2plug.in/ns/lv2core#minimum", "0", NULL);
        jack_set_property(client, uuid2, "http://lv2plug.in/ns/lv2core#maximum", "10", NULL);
    }

    jack2spi->tmpSortArray = malloc(sizeof(float)*jack_get_buffer_size(client));

    // Set callbacks
    jack_set_buffer_size_callback(client, bufsize_callback, jack2spi);
    jack_set_process_callback(client, process_callback, jack2spi);

    // done
    jack_activate(client);
    fprintf(stdout, "All good, let's roll!\n");

    return EXIT_SUCCESS;

    // TODO do something with `load_init`
    (void)load_init;
}

JACK_LIB_EXPORT
void jack_finish(void* arg)
{
    jack2spi_t* const jack2spi = (jack2spi_t*)arg;

    jack2spi->run = false;
    jack_deactivate(jack2spi->client);

    pthread_join(jack2spi->thread, NULL);
    fclose(jack2spi->out1f);
    fclose(jack2spi->out2f);
    snd_mixer_close(jack2spi->mixer);
    sem_destroy(&jack2spi->sem);

    jack_port_unregister(jack2spi->client, jack2spi->port1);
    jack_port_unregister(jack2spi->client, jack2spi->port2);
    free(jack2spi);
}

int main(int argc, char* argv[])
{
    jack_client_t* const client = jack_client_open("mod-jack2spi", JackNoStartServer, NULL);

    if (!client)
    {
        fprintf(stderr, "Opening client failed.\n");
        return EXIT_FAILURE;
    }

    if (jack_initialize(client, argc > 1 ? argv[1] : "") != EXIT_SUCCESS)
        return EXIT_FAILURE;

    while (1)
        sleep(1);

    jack_finish(client);
    return EXIT_SUCCESS;
}
