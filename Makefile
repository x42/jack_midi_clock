CXXFLAGS=-ggdb -fno-inline -fno-default-inline
LIBS=-ljack

all: jack_midi_clock

jack_midi_clock: main.o
	g++ $(CXXFLAGS) $(LIBFLAGS) -o $@ $^ $(LIBS)
