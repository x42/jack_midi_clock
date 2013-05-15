PREFIX ?= /usr/local
bindir ?= $(PREFIX)/bin
mandir ?= $(PREFIX)/share/man

CXXFLAGS ?= -Wall -O3

###############################################################################

CXXFLAGS  += `pkg-config --cflags jack`
LOADLIBES  = `pkg-config --cflags --libs jack` -lm
man1dir    = $(mandir)/man1

###############################################################################

default: all

jack_midi_clock: jack_midi_clock.cpp

install-bin: jack_midi_clock
	install -d $(DESTDIR)$(bindir)
	install -m755 jack_midi_clock $(DESTDIR)$(bindir)

install-man: jack_midi_clock.1
	install -d $(DESTDIR)$(man1dir)
	install -m644 jack_midi_clock.1 $(DESTDIR)$(man1dir)

uninstall-bin:
	rm -f $(DESTDIR)$(bindir)/jack_midi_clock
	-rmdir $(DESTDIR)$(bindir)

uninstall-man:
	rm -f $(DESTDIR)$(man1dir)/jack_midi_clock.1
	-rmdir $(DESTDIR)$(man1dir)
	-rmdir $(DESTDIR)$(mandir)

clean:
	rm -f jack_midi_clock

all: jack_midi_clock

install: install-bin install-man

uninstall: uninstall-bin uninstall-man

.PHONY: default all clean install install-bin install-man uninstall uninstall-bin uninstall-man
