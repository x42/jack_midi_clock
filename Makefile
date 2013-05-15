PREFIX ?= /usr/local
CXXFLAGS ?= -ggdb -Wall -O3

###############################################################################

BINDIR = $(PREFIX)/bin
CXXFLAGS += `pkg-config --cflags jack`
LOADLIBES = `pkg-config --cflags --libs jack`

default: all

all: jack_midi_clock

jack_midi_clock: jack_midi_clock.cpp

install: jack_midi_clock
	install -d $(DESTDIR)$(BINDIR)
	install -m755 jack_midi_clock $(DESTDIR)$(BINDIR)

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/jack_midi_clock
	-rmdir $(DESTDIR)$(BINDIR)

clean:
	rm -f jack_midi_clock

.PHONY: default all clean install uninstall
