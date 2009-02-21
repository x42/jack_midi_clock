
// Code starting point was midiseq.c from the JACK sources.



#include <jack/jack.h>
#include <jack/midiport.h>
#include <iostream>
#include <cassert>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

using namespace std;

#define APPNAME "Jack MIDI Clock"

class JackMidiClock
{
public:
    JackMidiClock(void);
    ~JackMidiClock();

    int process(jack_nframes_t nframes);
    static int process_callback(jack_nframes_t nframes, void *arg);
    bool good(void) { return m_good; }

private:
    jack_client_t* m_client;
    jack_port_t* m_port;
    bool m_good;
};

#define CLIENT_FAILURE(msg) {						\
	cerr << "Could not connect to JACK server (" << msg << ")"	\
	     << endl;							\
	if (client) {							\
	    cerr << "...but JACK returned a non-null pointer?" << endl;	\
	    (client) = 0;						\
	}								\
	if (tries) cerr << "...trying again." << endl;			\
    }


#define CLIENT_SUCCESS(msg) {			\
	assert(client);				\
	cout << msg << endl;			\
	tries = 0;				\
    }

JackMidiClock::JackMidiClock(void) :
    m_client(0),
    m_port(0),
    m_good(true)
{
    jack_client_t* client;
    jack_port_t* port;
    int rv;
    const char* cname;

    jack_status_t status;
    int tries = 2;  // Sometimes jackd doesn't stop and start fast enough.
    while ( tries > 0 ) {
	--tries;
	client = jack_client_open(
	    APPNAME,
	    JackNullOption,
	    &status);
	switch(status) {
	case JackFailure:
	    CLIENT_FAILURE("unknown error");
	    break;
	case JackInvalidOption:
	    CLIENT_FAILURE("invalid option");
	    break;
	case JackNameNotUnique:
	    if (client) {
		cname = jack_get_client_name(client);
		CLIENT_SUCCESS(cname);
	    } else {
		CLIENT_FAILURE("name not unique");
	    }
	    break;
	case JackServerStarted:
	    CLIENT_SUCCESS("JACK Server started for Hydrogen.");
	    break;
	case JackServerFailed:
	    CLIENT_FAILURE("unable to connect");
	    break;
	case JackServerError:
	    CLIENT_FAILURE("communication error");
	    break;
	case JackNoSuchClient:
	    CLIENT_FAILURE("unknown client type");
	    break;
	case JackLoadFailure:
	    CLIENT_FAILURE("can't load internal client");
	    break;
	case JackInitFailure:
	    CLIENT_FAILURE("can't initialize client");
	    break;
	case JackShmFailure:
	    CLIENT_FAILURE("unable to access shared memory");
	    break;
	case JackVersionError:
	    CLIENT_FAILURE("client/server protocol version mismatch");
	default:
	    if (status) {
		cerr << "Unknown status with JACK server." << endl;
		if (client) {
		    CLIENT_SUCCESS("Client pointer is *not* null..."
				   " assuming we're OK");
		}
	    } else {
		CLIENT_SUCCESS("Connected to JACK server");
	    }				
	}
    }

    if(client == 0) {
	m_good = false;
	return;
    }
    m_client = client;

    port = jack_port_register(m_client,
			      "midi_out",
			      JACK_DEFAULT_MIDI_TYPE,
			      JackPortIsOutput,
			      0);

    if(port == 0) {
	cerr << "Could not register port." << endl;
	m_good = false;
	return;
    }
    m_port = port;

    rv = jack_set_process_callback(m_client,
				   JackMidiClock::process_callback,
				   static_cast<void*>(this));
    if(rv) {
	cerr << "Could not set process callback." << endl;
	m_good = false;
	return;
    }

    rv = jack_activate(m_client);
    if(rv) {
	cerr << "Could not activate the client." << endl;
	m_good = false;
	return;
    }
	
    return;
}

JackMidiClock::~JackMidiClock(void)
{
    if(m_client) {
	jack_deactivate(m_client);
    }
    if(m_port && m_client) {
	jack_port_unregister(m_client, m_port);
    }
    if(m_client) {
	jack_client_close(m_client);
    }
    return;
}

int JackMidiClock::process(jack_nframes_t nframes)
{
    int i,j;
    void* port_buf = jack_port_get_buffer(m_port, nframes);
    unsigned char* buffer;
    jack_midi_clear_buffer(port_buf);
    static bool toggle = false;

    if(toggle) {
	buffer = jack_midi_event_reserve(port_buf, 0, 1);
	buffer[0] = 0xF8;  // MIDI Clock Pulse
	toggle = false;
    } else {
	toggle = true;
    }

    return 0;
}

int JackMidiClock::process_callback(jack_nframes_t nframes, void *arg) {
    return static_cast<JackMidiClock*>(arg)->process(nframes);
}

int main(int argc, char **argv)
{
    JackMidiClock jmc;

    while(jmc.good())
    {
	sleep(1);
    }

    return 0;
}
