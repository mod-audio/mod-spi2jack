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

#include <jack/jack.h>
#include <jack/metadata.h>
#include <jack/uuid.h>

#include <pthread.h>

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "jackey.h"

// custom jack flag used for cv
// needed because we prefer jack2 which doesn't always have working metadata
#define JackPortIsControlVoltage 0x100

#define MAX_RAW_IIO_VALUE 4096

typedef struct {
  jack_client_t* client;
  jack_port_t* port1;
  jack_port_t* port2;
  float value1, value2;
  FILE *in1f, *in2f;
  volatile bool run, ready;
  pthread_t thread;
} spi2jack_t;

static inline float read_first_raw_spi_value(FILE* const f)
{
    char buf[64];

    if (fread(buf, sizeof(buf), 1, f) > 0 || feof(f)) {
        buf[sizeof(buf)-1] = '\0';
        return (float)atoi(buf) / MAX_RAW_IIO_VALUE;
    }

    return 0.0f;
}

void* read_spi_thread(void* ptr)
{
    spi2jack_t* const spi2jack = (spi2jack_t*)ptr;

    FILE* const in1f = spi2jack->in1f;
    FILE* const in2f = spi2jack->in2f;

    char buf[64];

    // first read
    spi2jack->value1 = read_first_raw_spi_value(in1f);
    spi2jack->value2 = read_first_raw_spi_value(in2f);
    spi2jack->ready = true;

    while (spi2jack->run) {
        usleep(50000); // 50ms

        rewind(in1f);

        if (fread(buf, sizeof(buf), 1, in1f) > 0 || feof(in1f)) {
            buf[sizeof(buf)-1] = '\0';
            spi2jack->value1 = (float)atoi(buf) / MAX_RAW_IIO_VALUE;
        }

        rewind(in2f);

        if (fread(buf, sizeof(buf), 1, in1f) > 0 || feof(in1f)) {
            buf[sizeof(buf)-1] = '\0';
            spi2jack->value2 = (float)atoi(buf) / MAX_RAW_IIO_VALUE;
        }
    }

    return NULL;
}

static int process_callback(jack_nframes_t nframes, void* arg)
{
    spi2jack_t* const spi2jack = (spi2jack_t*)arg;

    float* const port1buf = jack_port_get_buffer(spi2jack->port1, nframes);
    float* const port2buf = jack_port_get_buffer(spi2jack->port2, nframes);

    // FIXME this is awful!
    if (spi2jack->ready) {
        for (jack_nframes_t i=0; i<nframes; ++i) {
            port1buf[i] = spi2jack->value1;
            port2buf[i] = spi2jack->value2;
        }
    } else {
        memset(port1buf, 0, sizeof(float)*nframes);
        memset(port2buf, 0, sizeof(float)*nframes);
    }

    return 0;
}

JACK_LIB_EXPORT
int jack_initialize(jack_client_t* client, const char* load_init)
{
    FILE* const fname = fopen("/sys/bus/iio/devices/iio:device0/name", "rb");

    if (!fname) {
      fprintf(stderr, "Cannot get iio device\n");
      return EXIT_FAILURE;
    }

    char namebuf[32];
    if (fread(namebuf, sizeof(namebuf), 1, fname) == 0 && feof(fname) == 0) {
        fprintf(stderr, "Cannot read iio device name\n");
        return EXIT_FAILURE;
    }

    namebuf[sizeof(namebuf)-1] = '\0';
    namebuf[strlen(namebuf)-1] = '\0';
    fprintf(stdout, "Opening iio device '%s'...\n", namebuf);

    fclose(fname);

    FILE* const in1f = fopen("/sys/bus/iio/devices/iio:device0/in_voltage0_raw", "rb");
    if (!in1f) {
        fprintf(stderr, "Cannot get iio raw input 1 file\n");
        return EXIT_FAILURE;
    }

    FILE* const in2f = fopen("/sys/bus/iio/devices/iio:device0/in_voltage1_raw", "rb");
    if (!in2f) {
        fprintf(stderr, "Cannot get iio raw input 1 file\n");
        fclose(in2f);
        return EXIT_FAILURE;
    }

    spi2jack_t* const spi2jack = malloc(sizeof(spi2jack_t));
    if (!spi2jack) {
      fprintf(stderr, "Out of memory\n");
      return EXIT_FAILURE;
    }

    spi2jack->in1f = in1f;
    spi2jack->in2f = in2f;
    spi2jack->run = true;

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

    // Register ports.
    const long unsigned port_flags = JackPortIsTerminal|JackPortIsPhysical|JackPortIsOutput|JackPortIsControlVoltage;
    spi2jack->port1 = jack_port_register(client, "capture_1", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    spi2jack->port2 = jack_port_register(client, "capture_2", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);

    if (!spi2jack->port1 || !spi2jack->port2) {
        fprintf(stderr, "Can't register jack ports\n");
        fclose(in1f);
        fclose(in2f);
        free(spi2jack);
        return EXIT_FAILURE;
    }

    // Set port aliases and metadata
    jack_port_set_alias(spi2jack->port1, "CV Capture 1");
    jack_port_set_alias(spi2jack->port2, "CV Capture 2");

    const jack_uuid_t uuid1 = jack_port_uuid(spi2jack->port1);
    const jack_uuid_t uuid2 = jack_port_uuid(spi2jack->port2);

    if (!jack_uuid_empty(uuid1))
    {
        jack_set_property(client, uuid1, JACK_METADATA_PRETTY_NAME, "CV Capture 1", "text/plain");
        jack_set_property(client, uuid1, JACKEY_SIGNAL_TYPE, "CV", "text/plain");
        jack_set_property(client, uuid1, JACKEY_ORDER, "1", NULL);
    }

    if (!jack_uuid_empty(uuid2))
    {
        jack_set_property(client, uuid2, JACK_METADATA_PRETTY_NAME, "CV Capture 2", "text/plain");
        jack_set_property(client, uuid2, JACKEY_SIGNAL_TYPE, "CV", "text/plain");
        jack_set_property(client, uuid2, JACKEY_ORDER, "2", NULL);
    }

    // Set callbacks
    jack_set_process_callback(client, process_callback, spi2jack);

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
    spi2jack_t* const spi2jack = (spi2jack_t*)arg;

    spi2jack->run = false;
    jack_deactivate(spi2jack->client);

    pthread_join(spi2jack->thread, NULL);
    fclose(spi2jack->in1f);
    fclose(spi2jack->in2f);

    jack_port_unregister(spi2jack->client, spi2jack->port1);
    jack_port_unregister(spi2jack->client, spi2jack->port2);
    free(spi2jack);
}

int main()
{
    jack_client_t* const client = jack_client_open("mod-spi2jack", JackNoStartServer, NULL);

    if (!client)
    {
        fprintf(stderr, "Opening client failed.\n");
        return EXIT_FAILURE;
    }

    if (jack_initialize(client, "") != EXIT_SUCCESS)
        return EXIT_FAILURE;

    while (1)
        sleep(1);

    jack_finish(client);
    return EXIT_SUCCESS;
}
