Jack MIDI Clock
===============

This program is a [JACK](http://jackaudio.org/) client that sends MIDI
beat clock pulses based on the current tempo given by the JACK transport.


Dependencies
------------

*   Standard C development tools (cc, make, libc)
*   Jack Audio Connection Kit (libjack-dev)


Installation
------------

```bash
 git clone https://github.com/x42/jack_midi_clock.git
 cd jack_midi_clock
 make
 sudo make install
```

The makefile honors `CFLAGS`, `LDFLAGS`, `DESTDIR` and `PREFIX` variables.
e.g. `make install PREFIX=/usr` and also supports `uninstall` target as well as
individual `[un]install-bin`, `[un]install-man` targets.


Usage
-----

Start jackd and some application to act as JACK-timecode master, then launch:

```bash
 jack_midi_clock &
```

The application runs until it receives a TERM or QUIT signal or jackd terminates.

Also see `jack_midi_clock -h` or the included manual page.


License
-------

`jack_midi_clock` is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License version 2 or (at
your option) any later version.

Copyright (C) 2009 by Gabriel M. Beddingfield <gabriel@teuton.org>
Copyright (C) 2013 by Robin Gareus <robin@gareus.org>

See COPYING for the complete license text.


About JACK MIDI Clock
---------------------

Jack Midi Clock is free software written to glorify God and His Savior
Jesus Christ. I dedicate this code to spreading His word and His
love. If you have found this software useful, please make a donation
to a Christian missionary (of your choice).[1] Suggested donation is
US $10 or US $20.

Questions? Drop me a line: gabriel@teuton.org

Grace and peace to you all!

[1] Suggestion on missionaries: Wycliffe Bible Translators (www.wycliffe.org)


PS. Updates and fixes to the source as well as build-system - contributed by
R. Gareus in 2013 - were motivated by the superdirt.net live performance at the
Linux Audio Conference 2013.
Robin neither approves nor advises against monetary donations, but he
subscribes to the virtue of altruistic love and wishes you to do likewise.
