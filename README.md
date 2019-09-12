mod-spi2jack
============

About
-----

mod-spi2jack is a JACK client for converting SPI (used for ADC and DAC) to a JACK control-voltage stream and vice-versa.

It was created mainly to be used on the MOD Duo X unit, reading values from its chips and converting as needed for JACK.

mod-spi2jack is part of the [MOD project](http://moddevices.com).


Building
--------

mod-spi2jack uses a simple Makefile to build the source code.
The steps to build and install are:

    make
    make install

You can change the base installation path passing PREFIX as argument of make.
There are no build dependencies besides JACK itself.

Running
-------

For developing start it like this:

    $ ./mod-spi2jack /dev/iio...

mod-spi2jack does not startup JACK automatically, so you need to start it before running mod-spi2jack.
