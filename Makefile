CXXFLAGS += -ggdb -fno-inline -fno-default-inline
JACK_FLAGS = `pkg-config --cflags --libs jack`

all: jack_midi_clock

jack_midi_clock: main.o
	g++ $(CXXFLAGS) $(LIBFLAGS) -o $@ $^ $(LIBS) $(JACK_FLAGS)

clean:
	rm -f jack_midi_clock main.o
