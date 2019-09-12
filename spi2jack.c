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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/metadata.h>
#include <jack/uuid.h>

#include "jackey.h"

// custom jack flag used for cv
// needed because we prefer jack2 which doesn't always have working metadata
#define JackPortIsControlVoltage 0x100

typedef struct {
  jack_client_t* client;
  jack_port_t* port1;
  jack_port_t* port2;
} spi2jack_t;

static int process_callback(jack_nframes_t nframes, void *arg)
{
    spi2jack_t* const spi2jack = (spi2jack_t*)arg;

    float* const port1buf = jack_port_get_buffer(spi2jack->port1, nframes);
    float* const port2buf = jack_port_get_buffer(spi2jack->port2, nframes);

    // TODO
    memset(port1buf, 0, sizeof(float)*nframes);
    memset(port2buf, 0, sizeof(float)*nframes);

    return 0;
}

JACK_LIB_EXPORT
int jack_initialize(jack_client_t* client, const char* load_init)
{
    spi2jack_t* const spi2jack = malloc(sizeof(spi2jack_t));
    if (!spi2jack) {
      fprintf(stderr, "Out of memory\n");
      return EXIT_FAILURE;
    }

    spi2jack->client = client;

    // Register ports.
    const long unsigned port_flags = JackPortIsTerminal|JackPortIsPhysical|JackPortIsOutput|JackPortIsControlVoltage;
    spi2jack->port1 = jack_port_register(client, "capture_1", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    spi2jack->port2 = jack_port_register(client, "capture_2", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);

    if (!spi2jack->port1 || !spi2jack->port2) {
        fprintf(stderr, "Can't register jack ports\n");
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
    return EXIT_SUCCESS;

    // TODO do something with `load_init`
    (void)load_init;
}

JACK_LIB_EXPORT
void jack_finish(void* arg)
{
    spi2jack_t* const spi2jack = (spi2jack_t*)arg;

    jack_deactivate(spi2jack->client);
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
