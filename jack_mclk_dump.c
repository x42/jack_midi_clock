/* JACK MIDI Beat Clock Parser
 *
 * (C) 2013  Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

//#define JACK_TRANSPORT_SYNC_CHECK // not rt-safe

#ifdef WIN32
#include <windows.h>
#include <pthread.h>
#define pthread_t //< override jack.h def
#endif

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <math.h>
#include <sys/mman.h>

#ifndef WIN32
#include <signal.h>
#include <pthread.h>
#endif

#include <jack/jack.h>
#include <jack/transport.h>
#include <jack/ringbuffer.h>
#include <jack/midiport.h>

#define RBSIZE 20
#define METRUM (4) // TODO allow to configure.

typedef struct {
  uint8_t msg;
  int pos;
  unsigned long long int tme;
} timenfo;

typedef struct {
  double t0; ///< time of the current Mclk tick
  double t1; ///< expected next Mclk tick
  double e2; ///< second order loop error
  double b, c, omega; ///< DLL filter coefficients
} DelayLockedLoop;

struct appstate {
  timenfo pt; // previous timeinfo
  DelayLockedLoop dll;
  uint64_t transport; /// timestamp of transport start/continue, 0 if stopped
  uint64_t sequence; /// beat clock signals since transport-state change
  int bcnt;  /// last song position
};

/* jack connection */
jack_client_t *j_client = NULL;
jack_port_t   *mclk_input_port;

/* threaded communication */
static jack_ringbuffer_t *rb = NULL;
static pthread_mutex_t msg_thread_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t data_ready = PTHREAD_COND_INITIALIZER;

/* application state */
static double samplerate = 48000.0;
static volatile unsigned long long monotonic_cnt = 0;
static int run = 1;

/* options */
static char newline = '\r'; // or '\n';
static short keeplastclk = 1;  // print newline on events
static double dll_bandwidth = 6.0; // 1/Hz

static struct appstate state;
static void print_time_event(struct appstate *s, timenfo *t);

/**
 * parse Midi Beat Clock events
 * enqueue to ring buffer and wake-up 'dump' thread
 */
static void process_jmidi_event(jack_midi_event_t *ev, unsigned long long mfcnt) {
  timenfo tnfo;
  memset(&tnfo, 0, sizeof(timenfo));
  if (ev->size != 1 && !((ev->size == 3 && ev->buffer[0] == 0xf2 ))) return;

  switch(ev->buffer[0]) {
    case 0xf2: // position
      tnfo.pos = (ev->buffer[2]<<7) | ev->buffer[1];
      break;
    case 0xf8: // clock
    case 0xfa: // start
    case 0xfb: // continue
    case 0xfc: // stop
      break;
    default:
      return;
  }

  tnfo.msg = ev->buffer[0];
  tnfo.tme = mfcnt + ev->time;
#ifdef JACK_TRANSPORT_SYNC_CHECK
  print_time_event(&state, &tnfo);
#else
  if (jack_ringbuffer_write_space(rb) >= sizeof(timenfo)) {
    jack_ringbuffer_write(rb, (void *) &tnfo, sizeof(timenfo));
  }

  if (pthread_mutex_trylock (&msg_thread_lock) == 0) {
    pthread_cond_signal (&data_ready);
    pthread_mutex_unlock (&msg_thread_lock);
  }
#endif
}

/**
 * jack process callback
 */
static int process(jack_nframes_t nframes, void *arg) {
  void *jack_buf = jack_port_get_buffer(mclk_input_port, nframes);
  int nevents = jack_midi_get_event_count(jack_buf);
  int n;

  for (n=0; n < nevents; n++) {
    jack_midi_event_t ev;
    jack_midi_event_get(&ev, jack_buf, n);
    process_jmidi_event(&ev, monotonic_cnt);
  }
  monotonic_cnt += nframes;
  return 0;
}

/**
 * callback if jack server terminates
 */
void jack_shutdown(void *arg) {
  j_client=NULL;
  pthread_cond_signal (&data_ready);
  fprintf (stderr, "jack server shutdown\n");
}

/**
 * cleanup and exit
 * call this function only after everything has been initialized!
 */
void cleanup(void) {
  if (j_client) {
    jack_deactivate (j_client);
    jack_client_close (j_client);
  }
  if (rb) {
    jack_ringbuffer_free(rb);
  }
  j_client = NULL;
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
  samplerate = (double) jack_get_sample_rate (j_client);

  return (0);
}

static int jack_portsetup(void) {
  if ((mclk_input_port = jack_port_register(j_client, "mclk_in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0)) == 0) {
    fprintf (stderr, "cannot register mclk input port !\n");
    return (-1);
  }
  return (0);
}

static void port_connect(char *mclk_port) {
  if (mclk_port && jack_connect(j_client, mclk_port, jack_port_name(mclk_input_port))) {
    fprintf(stderr, "cannot connect port %s to %s\n", mclk_port, jack_port_name(mclk_input_port));
  }
}

/**
 * initialize DLL
 * set current time and period in samples
 */
static void init_dll(DelayLockedLoop *dll, double tme, double period) {
  const double omega = 2.0 * M_PI * period / dll_bandwidth / samplerate;
  dll->b = 1.4142135623730950488 * omega;
  dll->c = omega * omega;

  dll->e2 = period / samplerate;
  dll->t0 = tme / samplerate;
  dll->t1 = dll->t0 + dll->e2;
}

/**
 * run one loop iteration.
 * @param tme time of event (in samples)
 * @return smoothed interval (period) [1/Hz]
 */
static double run_dll(DelayLockedLoop *dll, double tme) {
  const double e = tme / samplerate - dll->t1;
  dll->t0 = dll->t1;
  dll->t1 += dll->b * e + dll->e2;
  dll->e2 += dll->c * e;
  return (dll->t1 - dll->t0);
}

const char *msg_to_string(uint8_t msg) {
  switch(msg) {
    case 0xf8: return "clk";
    case 0xfa: return "start";
    case 0xfb: return "continue";
    case 0xfc: return "stop";
    default: return "??";
  }
}

#ifdef JACK_TRANSPORT_SYNC_CHECK
static const char *jt_to_string(jack_transport_state_t jts) {
  switch(jts) {
    case JackTransportStopped:  return ".";
    case JackTransportRolling:  return ">";
    case JackTransportStarting: return "*";
    default: return "?";
  }
}

static void print_jt(jack_transport_state_t jts, jack_position_t *jtpos) {
    if(jtpos->valid & JackPositionBBT) {
      printf(" %s:%4d|%d|%d",
	  jt_to_string(jts),
	  jtpos->bar, jtpos->beat,
	  (int) floor(4.0 * jtpos->tick / (double) jtpos->ticks_per_beat));
    } else {
      printf(" X:----|-|-");
    }
}
#endif

static void print_time_event(struct appstate *s, timenfo *t) {
  double flt_bpm = 0;
#ifdef JACK_TRANSPORT_SYNC_CHECK
    jack_position_t jtpos;
    jack_transport_state_t jts = jack_transport_query(j_client, &jtpos);
#endif

  if (t->msg == 0xf2) {
    /* song position */
    s->bcnt = t->pos;

    if (newline == '\r' && keeplastclk) printf("\n");
    fprintf(stdout, "POS (0x%04x) %4d.%d[beats] %4d|%d|%d [BBT@4/4] %-16s",
	t->pos,
	1 + t->pos/4, t->pos%4,
	1 + (t->pos/4/METRUM), 1 + ((t->pos/4)%METRUM), t->pos%4,
	"");
#ifdef JACK_TRANSPORT_SYNC_CHECK
      printf("           ");
#endif
    fprintf(stdout, " @ %lld       \n", t->tme);
  }
  else if (t->msg == 0xfa || t->msg == 0xfb || t->msg == 0xfc) {
    /* start, stop, continue -> reset */
    s->sequence = 0;
    if (t->msg == 0xfc) s->transport = 0; // stop
    else s->transport = t->tme;
    if (t->msg == 0xfa) s->bcnt = 0; // start

    if (newline == '\r' && keeplastclk) printf("\n");
    fprintf(stdout, "EVENT (0x%02x) %-49s",
	t->msg, msg_to_string(t->msg));
#ifdef JACK_TRANSPORT_SYNC_CHECK
      printf("           ");
#endif
    fprintf(stdout, " @ %lld       \n", t->tme);
  }
  else if (s->sequence == 1) {
    /* 2nd event in sequence -> initialize DLL with time difference */
    init_dll(&s->dll, t->tme, (t->tme - s->pt.tme));
    flt_bpm = samplerate * 60.0 / (24.0 * (double)(t->tme - s->pt.tme));
  }
  else if (s->sequence > 1) {
    /* run dll, calculate filtered bpm */
    flt_bpm = 60.0 / (24.0 * run_dll(&s->dll, t->tme));
  }

  /* print clock & bpm */
  if (t->msg == 0xf8 && s->sequence > 0) {
    const double samples_per_quarter_note = (t->tme - s->pt.tme) * 24.0;
    const double bpm = samplerate * 60.0 / samples_per_quarter_note;
    fprintf(stdout, "CLK cur: %7.2f[BPM] flt: %7.2f[BPM]  dt: %4lld[sm]", bpm, flt_bpm, (t->tme - s->pt.tme));
    if (s->transport) {
      int bp = s->bcnt + s->sequence / 6;
      printf(" %4d|%d|%d", 1 + (bp/4/METRUM), 1 + ((bp/4)%METRUM), bp%4);
    } else {
      printf(" ----|-|-");
    }
#ifdef JACK_TRANSPORT_SYNC_CHECK
    print_jt(jts, &jtpos);
#endif
    fprintf(stdout, " @ %lld       %c", t->tme, newline);
  } else if (t->msg == 0xf8) {
    fprintf(stdout, "CLK cur:      ??[BPM] flt:      ??[BPM]  dt:   ??[sm]         ");
#ifdef JACK_TRANSPORT_SYNC_CHECK
    print_jt(jts, &jtpos);
#endif
    fprintf(stdout, " @ %lld       %c", t->tme, newline);
  }

  if (t->msg == 0xf8) {
    memcpy(&s->pt, t, sizeof(timenfo));
    s->sequence++;
  }
}


/* TODO: it's not safe to call pthread_cond_signal from a signal handler
 * See http://pubs.opengroup.org/onlinepubs/009695399/functions/pthread_cond_broadcast.html
 */
static void wearedone(int sig) {
  fprintf(stderr,"caught signal - shutting down.\n");
  run=0;
  pthread_cond_signal (&data_ready);
}


/**************************
 * main application code
 */

static struct option const long_options[] =
{
  {"bandwidth", required_argument, 0, 'b'},
  {"help", no_argument, 0, 'h'},
  {"newline", no_argument, 0, 'n'},
  {"version", no_argument, 0, 'V'},
  {NULL, 0, NULL, 0}
};

static void usage (int status) {
  printf ("jack_mclk_dump - JACK MIDI Clock dump.\n\n");
  printf ("Usage: jack_mclk_dump [ OPTIONS ] [JACK-port]\n\n");
  printf ("Options:\n\
  -b, --bandwidth <1/Hz>     DLL bandwidth in 1/Hz (default: 6.0)\n\
  -h, --help                 display this help and exit\n\
  -n, --newline              print a newline after each Tick\n\
  -V, --version              print version information and exit\n\
\n");
  printf ("\n\
This tool subscribes to a JACK Midi Port and prints received Midi\n\
beat clock and BPM to stdout.\n\
\n\
See also: jack_midi_clock(1)\n\
\n");
  printf ("Report bugs to Robin Gareus <robin@gareus.org>\n"
	  "Website and manual: <https://github.com/x42/jack_midi_clock>\n"
    );
  exit (status);
}

static int decode_switches (int argc, char **argv) {
  int c;

  while ((c = getopt_long (argc, argv,
	 "b:" /* bandwidth */
	 "h"  /* help */
	 "n"  /* newline */
	 "V", /* version */
	 long_options, (int *) 0)) != EOF) {
    switch (c) {
      case 'b':
	dll_bandwidth = atof(optarg);
	if (dll_bandwidth < 0.1 || dll_bandwidth > 100.0) {
	  fprintf(stderr, "Invalid bandwidth, should be 0.1 <= bw <= 100.0. Using 6.0sec\n");
	  dll_bandwidth = 6.0;
	}
	break;
      case 'n':
	newline = '\n';
	break;
      case 'V':
	printf ("jack_mclk_dump version %s\n\n", VERSION);
	printf ("Copyright (C) GPL 2013 Robin Gareus <robin@gareus.org>\n");
	exit (0);

      case 'h':
	usage (0);

      default:
	usage (EXIT_FAILURE);
    }
  }
  return optind;
}

int main (int argc, char ** argv) {
  int i;

  decode_switches (argc, argv);

  if (init_jack("jack_mclk_dump"))
    goto out;
  if (jack_portsetup())
    goto out;

  rb = jack_ringbuffer_create(RBSIZE * sizeof(timenfo));

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
  signal(SIGHUP, wearedone);
  signal(SIGINT, wearedone);
#endif

  memset(&state, 0, sizeof(struct appstate));
  pthread_mutex_lock (&msg_thread_lock);

  /* all systems go */

  while (run && j_client) {
    const int mqlen = jack_ringbuffer_read_space (rb) / sizeof(timenfo);
    for (i=0; i < mqlen; ++i) {
      /* process Mclk event */
      timenfo t;
      jack_ringbuffer_read(rb, (char*) &t, sizeof(timenfo));
      print_time_event(&state, &t);
    }
    fflush(stdout);
    pthread_cond_wait (&data_ready, &msg_thread_lock);
  }
  pthread_mutex_unlock (&msg_thread_lock);

out:
  cleanup();
  return 0;
}
/* vi:set ts=8 sts=2 sw=2: */
