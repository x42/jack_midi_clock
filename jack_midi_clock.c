/* JACK-Transport MIDI Beat Clock Generator
 *
 * Copyright (C) 2013 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2009 Gabriel M. Beddingfield <gabriel@teuton.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <getopt.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#include <sys/mman.h>

#ifndef WIN32
#include <signal.h>
#endif

/* bitwise flags -- used w/ msg_filter */
enum {
  MSG_NO_TRANSPORT  = 1, /**< do not send start/stop/continue messages */
  MSG_NO_POSITION   = 2  /**< do not send absolute song position */
};

/* jack_position_t - excerpt */
struct bbtpos {
  jack_position_bits_t valid;  /**< which other fields are valid */
  int32_t   bar;            /**< current bar */
  int32_t   beat;           /**< current beat-within-bar */
  int32_t   tick;           /**< current tick-within-beat */
  double    bar_start_tick; /**< number of ticks that have elapsed between frame 0 and the first beat of the current measure. */
};

/* jack connection */
static jack_port_t            *mclk_output_port = NULL;
static jack_client_t          *j_client = NULL;

/* application state */
static jack_transport_state_t  m_xstate = JackTransportStopped;
static double                  mclk_last_tick = 0.0;
static int64_t                 song_position_sync = -1;
static struct bbtpos           last_xpos; /** keep track of transport locates */

static volatile enum {
  Init,
  Run,
  Exit
} client_state = Init;
static int wake_main_read = -1;
static int wake_main_write = -1;

/* commandline options */
static double   user_bpm   = 0.0;
static short    force_bpm  = 0;
static short    tempo_is_qnpm = 1;  /** tempo is quarter notes per minute instead of BPM */
static short    msg_filter = 0;     /** bitwise flags, MSG_NO_.. */
static double   resync_delay = 2.0; /**< seconds between 'pos' and 'continue' message */

/* MIDI System Real-Time Messages
 * https://en.wikipedia.org/wiki/MIDI_beat_clock
 * http://www.midi.org/techspecs/midimessages.php
 */
#define MIDI_RT_CLOCK    (0xF8)
#define MIDI_RT_START    (0xFA)
#define MIDI_RT_CONTINUE (0xFB)
#define MIDI_RT_STOP     (0xFC)


static void wake_main_init(void)
{
#ifndef WIN32
  int pipefd[2] = {-1, -1};
  if (pipe(pipefd) == -1) {
    fprintf(stderr, "Warning: unable to create pipe for signaling main thread.\n");
    return;
  }
  wake_main_read = pipefd[0];
  wake_main_write = pipefd[1];
#endif
}

/**
 * Wake the main thread (for shutdown)
 * Call this function when the main application needs to shut down.
 */
static void wake_main_now(void)
{
#ifndef WIN32
  char c = 0;
  write(wake_main_write, &c, sizeof(c));
#endif
}

/**
 * Wait for wake signal
 * This blocks until either a signal is received or a wake
 * message is received on the pipe.
 */
static void wake_main_wait(void)
{
#ifndef WIN32
  if (wake_main_read != -1) {
	  char c = 0;
	  read(wake_main_read, &c, sizeof(c));
  } else {
    /* fall back on using sleep if pipe fd is invalid */
    sleep(1);
  }
#else
  sleep(1);
#endif
}

/**
 * cleanup and exit
 * call this function only _after_ everything has been initialized!
 */
static void cleanup(int sig) {
  if (j_client) {
    jack_client_close (j_client);
    j_client = NULL;
  }
}

/**
 * compare two BBT positions
 */
static int pos_changed (struct bbtpos *xp0, jack_position_t *xp1) {
  if (!(xp0->valid & JackPositionBBT)) return -1;
  if (!(xp1->valid & JackPositionBBT)) return -2;
  if (   xp0->bar  == xp1->bar
      && xp0->beat == xp1->beat
      && xp0->tick == xp1->tick
     ) return 0;
  return 1;
}

/**
 * copy relevant BBT info from jack_position_t
 */
static void remember_pos (struct bbtpos *xp0, jack_position_t *xp1) {
  if (!(xp1->valid & JackPositionBBT)) return;
  xp0->valid = xp1->valid;
  xp0->bar   = xp1->bar;
  xp0->beat  = xp1->beat;
  xp0->tick  = xp1->tick;
  xp0->bar_start_tick = xp1->bar_start_tick;
}

/**
 * calculate song position (14 bit integer)
 * from current jack BBT info.
 *
 * see "Song Position Pointer" at
 * http://www.midi.org/techspecs/midimessages.php
 *
 * Because this value is also used internally to sync/send
 * start/continue realtime messages, a 64 bit integer
 * is used to cover the full range of jack transport.
 */
static const int64_t calc_song_pos(jack_position_t *xpos, int off) {
  if (!(xpos->valid & JackPositionBBT)) return -1;

  if (off < 0) {
    /* auto offset */
    if (xpos->bar == 1 && xpos->beat == 1 && xpos->tick == 0) off = 0;
    else off = rintf(xpos->beats_per_minute * 4.0 * resync_delay / 60.0);
  }

  /* MIDI Beat Clock: 24 ticks per quarter note
   * one MIDI-beat = six MIDI clocks
   * -> 4 MIDI-beats per quarter note (jack beat)
   * Note: jack counts bars and beats starting at 1
   */
  int64_t pos =
    off
    + 4 * ((xpos->bar - 1) * xpos->beats_per_bar + (xpos->beat - 1))
    + floor(4.0 * xpos->tick / xpos->ticks_per_beat);

  return pos;
}

static const int64_t send_pos_message(void* port_buf, jack_position_t *xpos, int off) {
  if (msg_filter & MSG_NO_POSITION) return -1;
  uint8_t *buffer;
  const int64_t bcnt = calc_song_pos(xpos, off);

  /* send '0xf2' Song Position Pointer.
   * This is an internal 14 bit register that holds the number of
   * MIDI beats (1 beat = six MIDI clocks) since the start of the song.
   */
  if (bcnt < 0 || bcnt >= 16384) {
    return -1;
  }

  buffer = jack_midi_event_reserve(port_buf, 0, 3);
  if(!buffer) {
    return -1;
  }
  buffer[0] = 0xf2;
  buffer[1] = (bcnt)&0x7f; // LSB
  buffer[2] = (bcnt>>7)&0x7f; // MSB
  return bcnt;
}

/**
 * send 1 byte MIDI Message
 * @param port_buf buffer to write event to
 * @param time sample offset of event
 * @param rt_msg message byte
 */
static void send_rt_message(void* port_buf, jack_nframes_t time, uint8_t rt_msg) {
  uint8_t *buffer;
  buffer = jack_midi_event_reserve(port_buf, time, 1);
  if(buffer) {
    buffer[0] = rt_msg;
  }
}

/**
 * jack process callback.
 * do the work: query jack-transport, send MIDI messages..
 */
static int process (jack_nframes_t nframes, void *arg) {
  jack_position_t xpos;
  double samples_per_beat;
  jack_nframes_t bbt_offset = 0;
  int ticks_sent_this_cycle = 0;

  /* query jack transport state */
  jack_transport_state_t xstate = jack_transport_query(j_client, &xpos);
  void* port_buf = jack_port_get_buffer(mclk_output_port, nframes);

  /* prepare MIDI buffer */
  jack_midi_clear_buffer(port_buf);

  if (client_state != Run) {
    return 0;
  }

  /* send position updates if stopped and located */
  if (xstate == JackTransportStopped && xstate == m_xstate) {
    if (pos_changed(&last_xpos, &xpos) > 0) {
      song_position_sync = send_pos_message(port_buf, &xpos, -1);
    }
  }
  remember_pos(&last_xpos, &xpos);

  /* send RT messages start/stop/continue if transport state changed */
  if( xstate != m_xstate ) {
    switch(xstate) {
      case JackTransportStopped:
	if (!(msg_filter & MSG_NO_TRANSPORT)) {
	  send_rt_message(port_buf, 0, MIDI_RT_STOP);
	}
	song_position_sync = send_pos_message(port_buf, &xpos, -1);
	break;
      case JackTransportRolling:
	/* handle transport locate while rolling.
	 * jack transport state changes  Rolling -> Starting -> Rolling
	 */
	if(m_xstate == JackTransportStarting && !(msg_filter & MSG_NO_POSITION)) {
	  if (song_position_sync < 0) {
	    /* send stop IFF not stopped, yet */
	    send_rt_message(port_buf, 0, MIDI_RT_STOP);
	  }
	  if (song_position_sync != 0) {
	    /* re-set 'continue' message sync point */
	    if ((song_position_sync = send_pos_message(port_buf, &xpos, -1)) < 0) {
	      if (!(msg_filter & MSG_NO_TRANSPORT)) {
		send_rt_message(port_buf, 0, MIDI_RT_CONTINUE);
	      }
	    }
	  } else {
	    /* 'start' at 0, don't queue 'continue' message */
	    song_position_sync = -1;
	  }
	  break;
	}
      case JackTransportStarting:
	if(m_xstate == JackTransportStarting) {
	  break;
	}
	if( xpos.frame == 0 ) {
	  if (!(msg_filter & MSG_NO_TRANSPORT)) {
	    send_rt_message(port_buf, 0, MIDI_RT_START);
	    song_position_sync = 0;
	  }
	} else {
	  /* only send continue message here if song-position
	   * is not used .
	   * w/song-pos it queued just-in-time
	   */
	  if (!(msg_filter & MSG_NO_TRANSPORT) && (msg_filter & MSG_NO_POSITION)) {
	    send_rt_message(port_buf, 0, MIDI_RT_CONTINUE);
	  }
	}
	break;
      default:
	break;
    }

    /* initial beat tick */
    if (xstate == JackTransportRolling
	&& ((xpos.frame == 0) || (msg_filter & MSG_NO_POSITION))
	) {
      send_rt_message(port_buf, 0, MIDI_RT_CLOCK);
    }

    mclk_last_tick = xpos.frame;
    m_xstate = xstate;
  }

  if((xstate != JackTransportRolling)) {
    return 0;
  }

  /* calculate clock tick interval */
  if(force_bpm && user_bpm > 0) {
    samples_per_beat = (double) xpos.frame_rate * 60.0 / user_bpm;
  }
  else if(xpos.valid & JackPositionBBT) {
    samples_per_beat = (double) xpos.frame_rate * 60.0 / xpos.beats_per_minute;
    if (xpos.valid & JackBBTFrameOffset) {
      bbt_offset = xpos.bbt_offset;
    }
  }
  else if(user_bpm > 0) {
    samples_per_beat = (double) xpos.frame_rate * 60.0 / user_bpm;
  } else {
    return 0; /* no tempo known */
  }

  /* It is an industry convention that tempo, while reported as "beats
   * per minute" is actually "quarter notes per minute" in many DAW's.
   * However, some DAW's/musicians actually use beats per minute
   * (using the definition of "beat" as the denomitor of the time
   * signature). While it appears that the JACK transport's intent
   * is the latter, it's totally up to the DAW to define the tempo/note
   * relationship. Currently Ardour does "quarter notes per minute."
   *
   * Viz. https://community.ardour.org/node/1433
   *      http://www.steinberg.net/forums/viewtopic.php?t=56065
   */
  const double quarter_notes_per_beat = (tempo_is_qnpm) ? 1.0 : (xpos.beat_type / 4.0);

  /* MIDI Beat Clock: Send 24 ticks per quarter note  */
  const double samples_per_quarter_note = samples_per_beat / quarter_notes_per_beat;
  const double clock_tick_interval = samples_per_quarter_note / 24.0;


  /* send clock ticks for this cycle */
  while(1) {
    const double next_tick = mclk_last_tick + clock_tick_interval;
    const int64_t next_tick_offset = llrint(next_tick) - xpos.frame - bbt_offset;
    if (next_tick_offset >= nframes) break;

    if (next_tick_offset >= 0) {

      if (song_position_sync > 0 && !(msg_filter & MSG_NO_POSITION)) {
	/* send 'continue' realtime message on time */
	const int64_t sync = calc_song_pos(&xpos, 0);
	/* 4 MIDI-beats per quarter note (jack beat) */
	if (sync + ticks_sent_this_cycle / 4 >= song_position_sync) {
	  if (!(msg_filter & MSG_NO_TRANSPORT)) {
	    send_rt_message(port_buf, next_tick_offset, MIDI_RT_CONTINUE);
	  }
	  song_position_sync = -1;
	}
      }

      /* enqueue clock tick */
      send_rt_message(port_buf, next_tick_offset, MIDI_RT_CLOCK);
    }

    mclk_last_tick = next_tick;
    ticks_sent_this_cycle++;
  }

  return 0;
}

/**
 * callback if jack server terminates
 */
static void jack_shutdown (void *arg) {
  fprintf(stderr, "recv. shutdown request from jackd.\n");
  client_state = Exit;
  wake_main_now();
}

/**
 * open a client connection to the JACK server
 */
static int init_jack(const char *client_name) {
  jack_status_t status;
  j_client = jack_client_open (client_name, JackNullOption, &status);
  if (j_client == NULL) {
    fprintf (stderr, "jack_client_open() failed, status = 0x%2.0x\n", status);
    if (status & JackServerFailed) {
      fprintf (stderr, "Unable to connect to JACK server\n");
    }
    return (-1);
  }
  if (status & JackServerStarted) {
    fprintf (stderr, "JACK server started\n");
  }
  if (status & JackNameNotUnique) {
    client_name = jack_get_client_name(j_client);
    fprintf (stderr, "jack-client name: `%s'\n", client_name);
  }

  jack_set_process_callback (j_client, process, 0);
#ifndef WIN32
  jack_on_shutdown (j_client, jack_shutdown, NULL);
#endif

  return (0);
}

static int jack_portsetup(void) {
  if ((mclk_output_port = jack_port_register(j_client, "mclk_out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0)) == 0) {
    fprintf (stderr, "cannot register mclk output port !\n");
    return (-1);
  }
  return (0);
}

static void port_connect(char *mclk_port) {
  if (mclk_port && jack_connect(j_client, jack_port_name(mclk_output_port), mclk_port)) {
    fprintf(stderr, "cannot connect port %s to %s\n", jack_port_name(mclk_output_port), mclk_port);
  }
}

static void catchsig (int sig) {
#ifndef _WIN32
  signal(SIGHUP, catchsig);
#endif
  client_state = Exit;
  wake_main_now();
}

/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"bpm", required_argument, 0, 'b'},
  {"force-bpm", no_argument, 0, 'B'},
  {"resync-delay", required_argument, 0, 'd'},
  {"help", no_argument, 0, 'h'},
  {"no-position", no_argument, 0, 'P'},
  {"no-transport", no_argument, 0, 'T'},
  {"strict-bpm", no_argument, 0, 's'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jack_midi_clock - JACK app to generate MCLK from JACK transport.\n\n");
  printf ("Usage: jack_midi_clock [ OPTIONS ] [JACK-port]*\n\n");
  printf ("Options:\n"

"  -b <bpm>, --bpm <bpm>\n"
"                         default BPM (if jack timecode master in not available)\n"
"  -B, --force-bpm        ignore jack timecode master\n"
"  -d <sec>, --resync-delay <sec>\n"
"                         seconds between 'song-position' and 'continue' message\n"
"  -P, --no-position      do not send song-position (0xf2) messages\n"
"  -T, --no-transport     do not send start/stop/continue messages\n"
"  -s, --strict-bpm       interpret tempo strictly as beats per minute (default\n"
"                         is quarter-notes per minute)\n"
"  -h, --help             display this help and exit\n"
"  -V, --version          print version information and exit\n"

"\n");
  printf ("\n"

/*                                  longest help text w/80 chars per line ---->|\n" */

"jack_midi_clock sends MIDI beat clock message if jack-transport is rolling.\n"
"it also sends start, continue and stop MIDI realtime messages whenever\n"
"the transport changes state (unless -T option is used).\n"
"\n"
"In order for jack_midi_clock to send clock messages, a JACK timecode master\n"
"must be present and provide the tempo map (bar, beat, tick).\n"
"Alternatively the -b option can be used to set a default BPM value.\n"
"If a value larger than zero is given, it will be used if no timecode master\n"
"is present. Combined with the -B option it can used to override and ignore\n"
"the JACK timecode master and only act on transport state alone.\n"
"\n"
"Either way, jack_midi_clock will never act as timecode master itself.\n"
"\n"
"Note that song-position information is only sent if a timecode master is\n"
"present ad the -P option is not given.\n"
"\n"
"To allow external synths to accurately sync to song-position, there is a two\n"
"second delay between the 'song-position changed' message (which is not a MIDI\n"
"realtime message) and the 'continue transport' message.\n"
"This delay can be configured with the -d option and is only relevant for if\n"
"playback starts at a bar|beat|tick other than 1|1|0 in which case a 'start'\n"
"message is sent immediately.\n"
"\n"
"jack_midi_clock runs until it receives a HUP or INT signal or jackd is\n"
"terminated.\n"
"\n"
"See also: jack_transport(1), jack_mclk_dump(1)\n"

"\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
          "Website: https://github.com/x42/jack_midi_clock/\n"
	  );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;

  while ((c = getopt_long (argc, argv,
			   "b:"	/* bpm */
			   "B"	/* force-bpm */
			   "d:"	/* resync-delay */
			   "h"	/* help */
			   "P"	/* no-position */
			   "T"	/* no-transport */
			   "s"  /* strict-bpm */
			   "V",	/* version */
			   long_options, (int *) 0)) != EOF)
    {
      switch (c) {
	case 'b':
	  user_bpm = atof(optarg);
	  break;

	case 'B':
	  force_bpm = 1;
	  break;

	case 'P':
	  msg_filter |= MSG_NO_POSITION;
	  break;

	case 'd':
	  resync_delay = atof(optarg);
	  if (resync_delay < 0 || resync_delay > 20) {
	    fprintf(stderr, "Invalid resync-delay, should be 0 <= dly <= 20.0. Using 2.0sec.\n");
	    resync_delay = 2.0;
	  }
	  break;

	case 'T':
	  msg_filter |= MSG_NO_TRANSPORT;
	  break;

        case 's':
          tempo_is_qnpm = 0;
          break;

	case 'V':
	  printf ("jack_midi_clock version %s\n\n", VERSION);
	  printf ("Copyright (C) GPL 2013 Robin Gareus <robin@gareus.org>\n");
	  printf ("Copyright (C) GPL 2009 Gabriel M. Beddingfield <gabriel@teuton.org>\n");
	  exit (0);

	case 'h':
	  usage (0);

	default:
	  usage (EXIT_FAILURE);
      }
    }

  return optind;
}

int main (int argc, char **argv) {
  memset(&last_xpos, 0, sizeof(struct bbtpos));

  decode_switches (argc, argv);

  if (init_jack("jack_midi_clock"))
    goto out;
  if (jack_portsetup())
    goto out;

  if (mlockall (MCL_CURRENT | MCL_FUTURE)) {
    fprintf(stderr, "Warning: Can not lock memory.\n");
  }

  if (jack_activate (j_client)) {
    fprintf (stderr, "cannot activate client.\n");
    goto out;
  }

  while (optind < argc)
    port_connect(argv[optind++]);

#ifndef _WIN32
  signal (SIGHUP, catchsig);
  signal (SIGINT, catchsig);
#endif

  wake_main_init();

  /* all systems go.
   * processs() does the work in jack realtime context
   */
  client_state = Run;
  while (client_state != Exit) {
    wake_main_wait();
  }

out:
  cleanup(0);
  return(0);
}

/* vi:set ts=8 sts=2 sw=2: */
