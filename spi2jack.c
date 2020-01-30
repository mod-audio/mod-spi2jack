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
#include <math.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#define ALSA_SOUNDCARD_DEFAULT_ID   "DUOX"
#define ALSA_CONTROL_CV_EXP_MODE    "CV/Exp.Pedal Mode"
#define ALSA_CONTROL_EXP_PEDAL_MODE "Exp.Pedal Mode"

// custom jack flag used for cv
// needed because we prefer jack2 which doesn't always have working metadata
#define JackPortIsControlVoltage 0x100

#define MAX_RAW_IIO_VALUE   4095
#define MAX_RAW_IIO_VALUE_f 4095.0f

typedef enum {
  exp_pedal_mode_unused,
  exp_pedal_mode_port1,
  exp_pedal_mode_port2
} exp_pedal_mode_t;

typedef struct {
  jack_client_t* client;
  jack_port_t* port1;
  jack_port_t* port2;
  jack_port_t* portPedal;
  float value1, value2;
  float prevvalue1, prevvalue2;
  FILE *in1f, *in2f;
  bool port_values_are_prescaled;
  volatile bool run, ready;
  volatile exp_pedal_mode_t exp_pedal_mode;
  pthread_t thread;
  jack_nframes_t bufsize_ns;
  float bufsize_log;
  // for knowing whichever exp.pedal mode we are on
  snd_mixer_t* mixer;
  snd_mixer_elem_t *mixerCvExpMode, *mixerExpPedalMode;
} spi2jack_t;

static bool _get_alsa_switch_value(snd_mixer_elem_t* const elem)
{
    int val = 0;
    snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_MONO, &val);
    return (val != 0);
}

static inline float read_first_raw_spi_value(FILE* const f)
{
    char buf[64];
    memset(buf, 0, sizeof(buf));

    if (fread(buf, sizeof(buf), 1, f) > 0 || feof(f))
    {
        buf[sizeof(buf)-1] = '\0';
        return (float)atoi(buf) / MAX_RAW_IIO_VALUE_f;
    }

    return 0.0f;
}

static void* read_spi_thread(void* ptr)
{
    spi2jack_t* const spi2jack = (spi2jack_t*)ptr;

    FILE* const in1f = spi2jack->in1f;
    FILE* const in2f = spi2jack->in2f;

    char buf[64];

    // first read
    spi2jack->prevvalue1 = spi2jack->value1 = read_first_raw_spi_value(in1f);
    spi2jack->prevvalue2 = spi2jack->value2 = read_first_raw_spi_value(in2f);
    spi2jack->ready = true;

    while (spi2jack->run)
    {
        usleep(spi2jack->bufsize_ns);

        rewind(in1f);
        memset(buf, 0, sizeof(buf));

        if (fread(buf, sizeof(buf), 1, in1f) > 0 || feof(in1f))
        {
            buf[sizeof(buf)-1] = '\0';
            spi2jack->value1 = (float)atoi(buf) / MAX_RAW_IIO_VALUE_f * 10.0f;
        }

        rewind(in2f);
        memset(buf, 0, sizeof(buf));

        if (fread(buf, sizeof(buf), 1, in2f) > 0 || feof(in2f))
        {
            buf[sizeof(buf)-1] = '\0';
            spi2jack->value2 = (float)atoi(buf) / MAX_RAW_IIO_VALUE_f * 10.0f;
        }

        // handle mixer changes
        if (spi2jack->mixer != NULL)
        {
            snd_mixer_handle_events(spi2jack->mixer);

            if (_get_alsa_switch_value(spi2jack->mixerCvExpMode))
            {
                if (_get_alsa_switch_value(spi2jack->mixerExpPedalMode))
                    spi2jack->exp_pedal_mode = exp_pedal_mode_port2;
                else
                    spi2jack->exp_pedal_mode = exp_pedal_mode_port1;
            }
            else
            {
                spi2jack->exp_pedal_mode = exp_pedal_mode_unused;
            }
        }
    }

    return NULL;
}

static int buffer_size_callback(jack_nframes_t bufsize, void* arg)
{
    spi2jack_t* const spi2jack = (spi2jack_t*)arg;

    spi2jack->bufsize_ns = bufsize / jack_get_sample_rate(spi2jack->client);
    spi2jack->bufsize_log = logf((float)bufsize);
    return 0;
}

/* python3
 import math
 for i in range(128):
     print("    %ff," % math.log(i+1))
*/
static float logfs[128] = {
    0.000000f,
    0.693147f,
    1.098612f,
    1.386294f,
    1.609438f,
    1.791759f,
    1.945910f,
    2.079442f,
    2.197225f,
    2.302585f,
    2.397895f,
    2.484907f,
    2.564949f,
    2.639057f,
    2.708050f,
    2.772589f,
    2.833213f,
    2.890372f,
    2.944439f,
    2.995732f,
    3.044522f,
    3.091042f,
    3.135494f,
    3.178054f,
    3.218876f,
    3.258097f,
    3.295837f,
    3.332205f,
    3.367296f,
    3.401197f,
    3.433987f,
    3.465736f,
    3.496508f,
    3.526361f,
    3.555348f,
    3.583519f,
    3.610918f,
    3.637586f,
    3.663562f,
    3.688879f,
    3.713572f,
    3.737670f,
    3.761200f,
    3.784190f,
    3.806662f,
    3.828641f,
    3.850148f,
    3.871201f,
    3.891820f,
    3.912023f,
    3.931826f,
    3.951244f,
    3.970292f,
    3.988984f,
    4.007333f,
    4.025352f,
    4.043051f,
    4.060443f,
    4.077537f,
    4.094345f,
    4.110874f,
    4.127134f,
    4.143135f,
    4.158883f,
    4.174387f,
    4.189655f,
    4.204693f,
    4.219508f,
    4.234107f,
    4.248495f,
    4.262680f,
    4.276666f,
    4.290459f,
    4.304065f,
    4.317488f,
    4.330733f,
    4.343805f,
    4.356709f,
    4.369448f,
    4.382027f,
    4.394449f,
    4.406719f,
    4.418841f,
    4.430817f,
    4.442651f,
    4.454347f,
    4.465908f,
    4.477337f,
    4.488636f,
    4.499810f,
    4.510860f,
    4.521789f,
    4.532599f,
    4.543295f,
    4.553877f,
    4.564348f,
    4.574711f,
    4.584967f,
    4.595120f,
    4.605170f,
    4.615121f,
    4.624973f,
    4.634729f,
    4.644391f,
    4.653960f,
    4.663439f,
    4.672829f,
    4.682131f,
    4.691348f,
    4.700480f,
    4.709530f,
    4.718499f,
    4.727388f,
    4.736198f,
    4.744932f,
    4.753590f,
    4.762174f,
    4.770685f,
    4.779123f,
    4.787492f,
    4.795791f,
    4.804021f,
    4.812184f,
    4.820282f,
    4.828314f,
    4.836282f,
    4.844187f,
    4.852030f
};

static inline
float calculate_jack_value_for_128_bufsize(float value, float prevvalue, jack_nframes_t i)
{
    const float multiplier = logfs[i] / 4.852030f;
    return value * multiplier + prevvalue * (1.0f - multiplier);
}

static inline
float calculate_jack_value(float value, float prevvalue, jack_nframes_t i, float bufsizelog)
{
    const float multiplier = logf((float)(i+1)) / bufsizelog;
    return value * multiplier + prevvalue * (1.0f - multiplier);
}

static int process_callback(jack_nframes_t nframes, void* arg)
{
    spi2jack_t* const spi2jack = (spi2jack_t*)arg;

    float* const port1buf = jack_port_get_buffer(spi2jack->port1, nframes);
    float* const port2buf = jack_port_get_buffer(spi2jack->port2, nframes);
    float* const portPbuf = jack_port_get_buffer(spi2jack->portPedal, nframes);

    if (spi2jack->ready)
    {
        const float value1     = spi2jack->value1;
        const float value2     = spi2jack->value2;
        const float prevvalue1 = spi2jack->prevvalue1;
        const float prevvalue2 = spi2jack->prevvalue2;
        const float epedalmult = spi2jack->port_values_are_prescaled ? 1.0f : 0.5f;
        spi2jack->prevvalue1   = value1;
        spi2jack->prevvalue2   = value2;

        switch (spi2jack->exp_pedal_mode)
        {
        case exp_pedal_mode_port1:
            // cv1
            memset(port1buf, 0, sizeof(float)*nframes);

            // cv2
            memset(port2buf, 0, sizeof(float)*nframes);

            // exp.pedal
            if (nframes == 128)
            {
                for (jack_nframes_t i=0; i<nframes; ++i)
                    portPbuf[i] = calculate_jack_value_for_128_bufsize(value1, prevvalue1, i) * epedalmult;
            }
            else
            {
                const float bufsizelog = spi2jack->bufsize_log;

                for (jack_nframes_t i=0; i<nframes; ++i)
                    portPbuf[i] = calculate_jack_value(value1, prevvalue1, i, bufsizelog) * epedalmult;
            }
            break;

        case exp_pedal_mode_port2:
            // cv1
            memset(port1buf, 0, sizeof(float)*nframes);

            // cv2
            memset(port2buf, 0, sizeof(float)*nframes);

            // exp.pedal
            if (nframes == 128)
            {
                for (jack_nframes_t i=0; i<nframes; ++i)
                    portPbuf[i] = calculate_jack_value_for_128_bufsize(value2, prevvalue2, i) * epedalmult;
            }
            else
            {
                const float bufsizelog = spi2jack->bufsize_log;

                for (jack_nframes_t i=0; i<nframes; ++i)
                    portPbuf[i] = calculate_jack_value(value2, prevvalue2, i, bufsizelog) * epedalmult;
            }
            break;

        default:
            // cv1 and 2
            if (nframes == 128)
            {
                for (jack_nframes_t i=0; i<nframes; ++i)
                {
                    port1buf[i] = calculate_jack_value_for_128_bufsize(value1, prevvalue1, i);
                    port2buf[i] = calculate_jack_value_for_128_bufsize(value2, prevvalue2, i);
                }
            }
            else
            {
                const float bufsizelog = spi2jack->bufsize_log;

                for (jack_nframes_t i=0; i<nframes; ++i)
                {
                    port1buf[i] = calculate_jack_value(value1, prevvalue1, i, bufsizelog);
                    port2buf[i] = calculate_jack_value(value2, prevvalue2, i, bufsizelog);
                }
            }

            // exp.pedal
            memset(portPbuf, 0, sizeof(float)*nframes);
            break;
        }
    }
    else
    {
        memset(port1buf, 0, sizeof(float)*nframes);
        memset(port2buf, 0, sizeof(float)*nframes);
        memset(portPbuf, 0, sizeof(float)*nframes);
    }

    return 0;
}

JACK_LIB_EXPORT
int jack_initialize(jack_client_t* client, const char* load_init);

JACK_LIB_EXPORT
void jack_finish(void* arg);

JACK_LIB_EXPORT
int jack_initialize(jack_client_t* client, const char* load_init)
{
    if (load_init == NULL || load_init[0] == '\0')
    {
        load_init = getenv("MOD_SPI2JACK_DEVICE");

        if (load_init == NULL || load_init[0] == '\0')
        {
          fprintf(stderr, "No spi device selected\n");
          return EXIT_FAILURE;
        }
    }

    char filename[512];
    memset(filename, 0, sizeof(filename));

    snprintf(filename, 511, "%s/name", load_init);
    FILE* const fname = fopen(filename, "rb");
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

    snprintf(filename, 511, "%s/in_voltage0_raw", load_init);
    FILE* const in1f = fopen(filename, "rb");
    if (!in1f)
    {
        fprintf(stderr, "Cannot get iio raw input 1 file\n");
        return EXIT_FAILURE;
    }

    snprintf(filename, 511, "%s/in_voltage1_raw", load_init);
    FILE* const in2f = fopen(filename, "rb");
    if (!in2f)
    {
        fprintf(stderr, "Cannot get iio raw input 1 file\n");
        fclose(in2f);
        return EXIT_FAILURE;
    }

    spi2jack_t* const spi2jack = calloc(sizeof(spi2jack_t), 1);
    if (!spi2jack)
    {
        fprintf(stderr, "Out of memory\n");
        return EXIT_FAILURE;
    }

    // FIXME better way to set this. for now, it works..
    spi2jack->port_values_are_prescaled = getenv("MOD_SPI2JACK_PRESCALED") != NULL;

    spi2jack->exp_pedal_mode = exp_pedal_mode_unused;
    spi2jack->in1f = in1f;
    spi2jack->in2f = in2f;
    spi2jack->run = true;

    // setup alsa-mixer listener
    if (snd_mixer_open(&spi2jack->mixer, SND_MIXER_ELEM_SIMPLE) == 0)
    {
        snd_mixer_selem_id_t* sid;

        char soundcard[32] = "hw:";

        const char* const cardname = getenv("MOD_SOUNDCARD");
        if (cardname != NULL)
            strncat(soundcard, cardname, 28);
        else
            strncat(soundcard, ALSA_SOUNDCARD_DEFAULT_ID, 28);

        soundcard[31] = '\0';

        if (snd_mixer_attach(spi2jack->mixer, soundcard) == 0 &&
            snd_mixer_selem_register(spi2jack->mixer, NULL, NULL) == 0 &&
            snd_mixer_load(spi2jack->mixer) == 0 &&
            snd_mixer_selem_id_malloc(&sid) == 0)
        {
            // CV/Exp Mode
            snd_mixer_selem_id_set_index(sid, 0);
            snd_mixer_selem_id_set_name(sid, ALSA_CONTROL_CV_EXP_MODE);
            spi2jack->mixerCvExpMode = snd_mixer_find_selem(spi2jack->mixer, sid);

            // Exp.Pedal Mode
            snd_mixer_selem_id_set_index(sid, 0);
            snd_mixer_selem_id_set_name(sid, ALSA_CONTROL_EXP_PEDAL_MODE);
            spi2jack->mixerExpPedalMode = snd_mixer_find_selem(spi2jack->mixer, sid);

            snd_mixer_selem_id_free(sid);
        }

        if (spi2jack->mixerCvExpMode == NULL || spi2jack->mixerExpPedalMode == NULL)
        {
            snd_mixer_close(spi2jack->mixer);
            spi2jack->mixer = NULL;
        }
    }

    // setup reading thread
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

    pthread_create(&spi2jack->thread, &attributes, read_spi_thread, (void*)spi2jack);
    pthread_attr_destroy(&attributes);

    spi2jack->client = client;

    const jack_nframes_t bufsize = jack_get_buffer_size(client);
    spi2jack->bufsize_ns = bufsize / jack_get_sample_rate(spi2jack->client);
    spi2jack->bufsize_log = logf((float)bufsize);

    // Register ports.
    const long unsigned port_flags = JackPortIsTerminal|JackPortIsPhysical|JackPortIsOutput|JackPortIsControlVoltage;
    spi2jack->port1     = jack_port_register(client, "capture_1", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    spi2jack->port2     = jack_port_register(client, "capture_2", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    spi2jack->portPedal = jack_port_register(client, "exp_pedal", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);

    if (!spi2jack->port1 || !spi2jack->port2 || !spi2jack->portPedal)
    {
        fprintf(stderr, "Can't register jack ports\n");
        fclose(in1f);
        fclose(in2f);
        free(spi2jack);
        return EXIT_FAILURE;
    }

    // Set port aliases and metadata
    jack_port_set_alias(spi2jack->port1,     "CV Capture 1");
    jack_port_set_alias(spi2jack->port2,     "CV Capture 2");
    jack_port_set_alias(spi2jack->portPedal, "Expression Pedal");

    const jack_uuid_t uuid1     = jack_port_uuid(spi2jack->port1);
    const jack_uuid_t uuid2     = jack_port_uuid(spi2jack->port2);
    const jack_uuid_t uuidPedal = jack_port_uuid(spi2jack->portPedal);

    if (!jack_uuid_empty(uuid1))
    {
        jack_set_property(client, uuid1, JACK_METADATA_PRETTY_NAME, "CV Capture 1", "text/plain");
        jack_set_property(client, uuid1, JACK_METADATA_SIGNAL_TYPE, "CV", "text/plain");
        jack_set_property(client, uuid1, JACK_METADATA_ORDER, "1", NULL);
        jack_set_property(client, uuid1, "http://lv2plug.in/ns/lv2core#minimum", "0", NULL);
        jack_set_property(client, uuid1, "http://lv2plug.in/ns/lv2core#maximum", "10", NULL);
    }

    if (!jack_uuid_empty(uuid2))
    {
        jack_set_property(client, uuid2, JACK_METADATA_PRETTY_NAME, "CV Capture 2", "text/plain");
        jack_set_property(client, uuid2, JACK_METADATA_SIGNAL_TYPE, "CV", "text/plain");
        jack_set_property(client, uuid2, JACK_METADATA_ORDER, "2", NULL);
        jack_set_property(client, uuid2, "http://lv2plug.in/ns/lv2core#minimum", "0", NULL);
        jack_set_property(client, uuid2, "http://lv2plug.in/ns/lv2core#maximum", "10", NULL);
    }

    if (!jack_uuid_empty(uuidPedal))
    {
        jack_set_property(client, uuidPedal, JACK_METADATA_PRETTY_NAME, "Expression Pedal", "text/plain");
        jack_set_property(client, uuidPedal, JACK_METADATA_SIGNAL_TYPE, "CV", "text/plain");
        jack_set_property(client, uuidPedal, JACK_METADATA_ORDER, "3", NULL);
        jack_set_property(client, uuidPedal, "http://lv2plug.in/ns/lv2core#minimum", "0", NULL);
        jack_set_property(client, uuidPedal, "http://lv2plug.in/ns/lv2core#maximum", "5", NULL);
    }

    // Set callbacks
    jack_set_buffer_size_callback(client, buffer_size_callback, spi2jack);
    jack_set_process_callback(client, process_callback, spi2jack);

    // done
    jack_activate(client);
    fprintf(stdout, "All good, let's roll!\n");

    return EXIT_SUCCESS;
}

JACK_LIB_EXPORT
void jack_finish(void* arg)
{
    spi2jack_t* const spi2jack = (spi2jack_t*)arg;

    spi2jack->run = false;
    jack_deactivate(spi2jack->client);

    pthread_join(spi2jack->thread, NULL);
    fclose(spi2jack->in1f);
    fclose(spi2jack->in2f);

    jack_port_unregister(spi2jack->client, spi2jack->port1);
    jack_port_unregister(spi2jack->client, spi2jack->port2);
    jack_port_unregister(spi2jack->client, spi2jack->portPedal);
    free(spi2jack);
}

int main(int argc, char* argv[])
{
    if (argc <= 1)
    {
        fprintf(stdout, "Usage: %s <bus-device>\n", argv[0]);
        fprintf(stdout, "\tWhere bus-device is something like '/sys/bus/iio/devices/iio:device0'\n");
        return EXIT_FAILURE;
    }

    jack_client_t* const client = jack_client_open("mod-spi2jack", JackNoStartServer, NULL);

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
