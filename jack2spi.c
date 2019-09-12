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
} jack2spi_t;

static int process_callback(jack_nframes_t nframes, void *arg)
{
    jack2spi_t* const jack2spi = (jack2spi_t*)arg;

    float* const port1buf = jack_port_get_buffer(jack2spi->port1, nframes);
    float* const port2buf = jack_port_get_buffer(jack2spi->port2, nframes);

    return 0;

    // TODO
    (void)port1buf;
    (void)port2buf;
}

JACK_LIB_EXPORT
int jack_initialize(jack_client_t* client, const char* load_init)
{
    jack2spi_t* const jack2spi = malloc(sizeof(jack2spi_t));
    if (!jack2spi) {
      fprintf(stderr, "Out of memory\n");
      return EXIT_FAILURE;
    }

    jack2spi->client = client;

    // Register ports.
    const long unsigned port_flags = JackPortIsTerminal|JackPortIsPhysical|JackPortIsInput|JackPortIsControlVoltage;
    jack2spi->port1 = jack_port_register(client, "playback_1", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);
    jack2spi->port2 = jack_port_register(client, "playback_2", JACK_DEFAULT_AUDIO_TYPE, port_flags, 0);

    if (!jack2spi->port1 || !jack2spi->port2) {
        fprintf(stderr, "Can't register jack ports\n");
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
        jack_set_property(client, uuid1, JACKEY_SIGNAL_TYPE, "CV", "text/plain");
        jack_set_property(client, uuid1, JACKEY_ORDER, "1", NULL);
    }

    if (!jack_uuid_empty(uuid2))
    {
        jack_set_property(client, uuid2, JACK_METADATA_PRETTY_NAME, "CV Playback 2", "text/plain");
        jack_set_property(client, uuid2, JACKEY_SIGNAL_TYPE, "CV", "text/plain");
        jack_set_property(client, uuid2, JACKEY_ORDER, "2", NULL);
    }

    // Set callbacks
    jack_set_process_callback(client, process_callback, jack2spi);

    // done
    jack_activate(client);
    return EXIT_SUCCESS;

    // TODO do something with `load_init`
    (void)load_init;
}

JACK_LIB_EXPORT
void jack_finish(void* arg)
{
    jack2spi_t* const jack2spi = (jack2spi_t*)arg;

    jack_deactivate(jack2spi->client);
    jack_port_unregister(jack2spi->client, jack2spi->port1);
    jack_port_unregister(jack2spi->client, jack2spi->port2);
    free(jack2spi);
}

int main()
{
    jack_client_t* const client = jack_client_open("mod-jack2spi", JackNoStartServer, NULL);

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
