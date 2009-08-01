
// Code starting point was midiseq.c from the JACK sources.



#include <jack/jack.h>
#include <jack/midiport.h>
#include <jack/transport.h>
#include <iostream>
#include <cassert>
#include <cmath>

#include <stdlib.h>
#include <stdint.h>  // e.g. for uint8_t
#include <signal.h>
#include <errno.h>

using namespace std;

#define APPNAME "Jack MIDI Clock"
#define APPVERSION "0.1.0"

class JackMidiClock
{
public:
    JackMidiClock(void);
    ~JackMidiClock();

    static int process_callback(jack_nframes_t nframes, void *arg);
    bool good(void) { return m_good; }
    void shutdown(void) { m_good = false; }

private:
    int process(jack_nframes_t nframes);
    void send_rt_message(void* port_buf, jack_nframes_t frame, uint8_t rt_msg);

    jack_client_t* m_client;
    jack_port_t* m_port;
    bool m_good;
    jack_transport_state_t m_xstate;

    // Cached calculations
    jack_nframes_t m_accum;     // Frame of last clock in last process()

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
    m_good(true),
    m_xstate(JackTransportStopped),
    m_accum(0)
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
	    CLIENT_SUCCESS("JACK Server started for " APPNAME);
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

/*
 * MIDI Clock is expected to send 24 pulses per beat.  A pulse
 * is sending a midi message: 'F8'
 *
 * This process callback will:
 *
 * o Send 0xF8 24 times per quarter note, sync'd to the beat, if the
 *   transport is rolling and the transport master is supplying BBT
 *   information.
 *
 * FUTURE PLANS:
 *
 * o Possibly continue sending pulses even when xport stopeed.
 *
 * o Have a default tempo that is sent out even if there is
 *   no transport master giving BBT info.
 *
 */
const uint8_t MIDI_RT_CLOCK = 0xF8;
// 0xF9 is undefined
const uint8_t MIDI_RT_START = 0xFA;
const uint8_t MIDI_RT_CONTINUE = 0xFB;
const uint8_t MIDI_RT_STOP = 0xFC;

int JackMidiClock::process(jack_nframes_t nframes)
{
    // See this->m_accum for state variable
    jack_position_t xpos;
    jack_transport_state_t xstate = jack_transport_query(m_client, &xpos);
    void* port_buf = jack_port_get_buffer(m_port, nframes);
    unsigned char* buffer;
    jack_midi_clear_buffer(port_buf);

    if( xstate != m_xstate ) {
	switch(xstate) {
	case JackTransportStopped:
	    send_rt_message(port_buf, 0, MIDI_RT_STOP);
	    #ifdef ENABLE_DEBUG
	    cout << "MIDI STOP" << endl;
	    #endif
	    break;
	case JackTransportStarting:
	case JackTransportRolling:
	    if( m_xstate == JackTransportStarting )
		break; // Should have already sent the start.
	    if( xpos.frame == 0 ) {
		send_rt_message(port_buf, 0, MIDI_RT_START);
		#ifdef ENABLE_DEBUG
		cout << "MIDI START" << endl;
		#endif
	    } else {
		send_rt_message(port_buf, 0, MIDI_RT_CONTINUE);
		#ifdef ENABLE_DEBUG
		cout << "MIDI CONTINUE" << endl;
		#endif
	    }
	    break;
	case JackTransportLooping:
	    // ignored
	    break;
	}
	m_xstate = xstate;
    }
    if( (xstate == JackTransportRolling)
	&& (xpos.valid & JackPositionBBT) ) {

	// Frame interval of 24 clocks per quarter note.
	double frames_per_beat =
	    double(xpos.frame_rate)
	    * 60.0
	    / xpos.beats_per_minute;

	double _interval =
	    frames_per_beat
	    * xpos.beat_type
	    / 4.0
	    / 24.0;

	jack_nframes_t interval = jack_nframes_t(round(_interval));
	jack_nframes_t fr = 0;

	#ifdef ENABLE_DEBUG
 	cout << " frame = " << (xpos.frame)
	     << " frame_rate = " << (xpos.frame_rate)
	     << " bpm = " << (xpos.beats_per_minute)
	     << " beat_type = " << (xpos.beat_type)
 	     << " m_accum = " << (m_accum)
 	     << " interval = " << (interval)
 	     << " nframes = " << (nframes)
 	     << endl;
	#endif
	if( m_accum > interval ) {
	    m_accum = interval;
	}
	fr = interval - m_accum;
	while( fr < nframes ) {
	    send_rt_message(port_buf, fr, MIDI_RT_CLOCK);
	    fr += interval;
	}
	if( fr >= nframes ) {
	    // last frame in this cycle = fr - interval
	    // incomplete frames in this cycle = nframes - (fr - interval)
	    m_accum = nframes - (fr - interval);
	} else {
	    m_accum += nframes;
	}
    }
    return 0;
}

void JackMidiClock::send_rt_message(void* port_buf, jack_nframes_t frame, uint8_t rt_msg)
{
    uint8_t *buffer;
    buffer = jack_midi_event_reserve(port_buf, frame, 1);
    if(buffer != 0) {
	buffer[0] = rt_msg;
    } else {
	cerr << "Could not write to buffer" << endl;
    }
}

int JackMidiClock::process_callback(jack_nframes_t nframes, void *arg) {
    return static_cast<JackMidiClock*>(arg)->process(nframes);
}

void about()
{
    cout << APPNAME << " version " << APPVERSION
	 << " Copyright (C) 2009 Gabriel M. Beddingfield" << endl
	 << APPNAME << " comes with ABSOLUTELY NO WARRANTY." << endl
	 << "This is free software, and you are welcome to redistribute it" << endl
	 << "under conditions of the GNU PUBLIC LICENSE (version 2 or later)." << endl
	 << endl;
}

JackMidiClock* jmc_instance = 0;

void sig_handler(int)
{
    if( jmc_instance ) {
	jmc_instance->shutdown();
    }
}

void setup_signal_handler(int signum)
{
    struct sigaction act;
    act.sa_handler = sig_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    act.sa_restorer = 0;

    if( sigaction( signum, &act, 0 ) ) {
	cerr << "Error:  could not set up signal handler for signal #" << signum << endl;
	switch( errno ) {
	case EFAULT:
	    cerr << "Invalid 'sigaction' structs given to sigaction()" << endl;
	    break;
	case EINVAL:
	    cerr << "Tried to set signal handler for invalid or unauthorized signal" << endl;
	    break;
	default:
	    cerr << "Unknown error #" << errno << endl;
	}	    
	exit(0);
    }
}

int main(int argc, char **argv)
{
    about();

    setup_signal_handler(SIGHUP);
    setup_signal_handler(SIGINT);
    setup_signal_handler(SIGQUIT);
    setup_signal_handler(SIGTERM);

    {
	JackMidiClock jmc;
	jmc_instance = &jmc;

	while(jmc.good())
	{
	    sleep(1);
	}
    }

    cout << "Disconnected from JACK server" << endl;
    return 0;
}
