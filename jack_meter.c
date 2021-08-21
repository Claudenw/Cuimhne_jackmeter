/*
    jack_meter.c
    LCD display based Digital Peak Meter for JACK on Cuimhne Ceoil
    Copyright (C) 2021 Claude Warren
    Based on

	jackmeter.c
	Simple console based Digital Peak Meter for JACK
	Copyright (C) 2005  Nicholas J. Humfrey
	
	This program is free software; you can redistribute it and/or
	modify it under the terms of the GNU General Public License
	as published by the Free Software Foundation; either version 2
	of the License, or (at your option) any later version.
	
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.
	
	You should have received a copy of the GNU General Public License
	along with this program; if not, write to the Free Software
	Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include <jack/jack.h>
#include <getopt.h>
#include "config.h"


float bias = 1.0f;
int decay_len;
char *server_name = NULL;

jack_client_t *client = NULL;
jack_options_t options = JackNoStartServer;

/* constants for lcd access */
#define DEFAULT_DEVICE "/dev/lcd0"
#define CONSOLE_WIDTH 20
#define DISPLAY_WIDTH (CONSOLE_WIDTH+6)
#define DISPLAY_SIZE(s) (s+6)

int xrun_count = 0;

#define DEFAULT_FIFO_NAME "/run/jack_meter"
char* fifo_name = NULL;
FILE *fifo = NULL;

int displaying = 0;
char* lcd_device = NULL;
char peak_char='I';
char meter_char='#';
int lcd;

/*
 * CHANNEL HANDLING
 */
#define MAX_CHANNELS 2
unsigned int channels = MAX_CHANNELS;
struct channel_info_t {
    int channel;
    int dpeak;
    int dtime;
    float peak;
    float last_peak;
    float db;
    jack_port_t *input_port;
} channel_info[MAX_CHANNELS];

/* DEBUG */

static unsigned int debug_level = 3;

static void debug( unsigned int level, const char *fmt, ...)
{
    va_list argp;
    if (level <= debug_level) {
        va_start(argp, fmt);
        vfprintf(stderr,fmt,argp);
        va_end(argp);
    }
}

/* Callback called by JACK when audio is available.
   Stores value of peak sample */
static int process_peak(jack_nframes_t nframes, void *arg)
{
	jack_default_audio_sample_t *in;
	unsigned int channel;
	unsigned int i;
	struct channel_info_t *info;
	for (channel = 0; channel < channels; channel++) {
	    info = &channel_info[channel];
        /* just incase the port isn't registered yet */
        if (info->input_port == NULL) {
            debug(2, "Channel %d ls\n", channel);
        } else {
            /* get the audio samples, and find the peak sample */
            in = (jack_default_audio_sample_t *) jack_port_get_buffer(info->input_port, nframes);
            for (i = 0; i < nframes; i++) {
                const float s = fabs(in[i]);
                if (s > info->peak) {
                    debug( 4, "Setting channel %d peak %f\n", channel, s );
                    info->peak = s;
                }
            }
        }
	}

	return 0;
}


/*
	db: the signal stength in db
	width: the size of the meter
*/
static int iec_scale(float db, int size) {
	float def = 0.0f; /* Meter deflection %age */
	
	if (db < -70.0f) {
		def = 0.0f;
	} else if (db < -60.0f) {
		def = (db + 70.0f) * 0.25f;
	} else if (db < -50.0f) {
		def = (db + 60.0f) * 0.5f + 2.5f;
	} else if (db < -40.0f) {
		def = (db + 50.0f) * 0.75f + 7.5;
	} else if (db < -30.0f) {
		def = (db + 40.0f) * 1.5f + 15.0f;
	} else if (db < -20.0f) {
		def = (db + 30.0f) * 2.0f + 30.0f;
	} else if (db < 0.0f) {
		def = (db + 20.0f) * 2.5f + 50.0f;
	} else {
		def = 100.0f;
	}
	
	return (int)( (def / 100.0f) * ((float) size) );
}


/* Connect the chosen port to ours */
static void connect_port(jack_client_t *client, char *port_name, unsigned int channel)
{
	jack_port_t *port;

	// Get the port we are connecting to
	port = jack_port_by_name(client, port_name);
	if (port == NULL) {
		debug( 1, "Can't find port '%s'\n", port_name);
		exit(1);
	}
	const char* fq_port_name = jack_port_name(port);
	const char* fq_channel_name = jack_port_name(channel_info[channel].input_port);
	// Connect the port to our input port
	debug( 4, "Connecting '%s' to '%s' on channel %d\n", fq_port_name, fq_channel_name, channel );
	if (jack_connect(client, fq_port_name, fq_channel_name)) {
		debug( 1, "Cannot connect '%s' to '%s' on channel %d\n", fq_port_name, fq_channel_name, channel);
		exit(1);
	}
}


/* Sleep for a fraction of a second */
static int fsleep( float secs )
{

//#ifdef HAVE_USLEEP
	return usleep( secs * 1000000 );
//#endif
}


/* Display how to use this program */
static int usage( const char * progname )
{
	fprintf(stderr, "jackmeter version %s\n\n", VERSION);
	fprintf(stderr, "Usage %s [-f freqency] [-r ref-level] [-w width] [-s servername] [-n] [<port>, ...]\n\n", progname);
	fprintf(stderr, "where  -f      is how often to update the meter per second [8]\n");
	fprintf(stderr, "       -d      is the debug level (0 = silent, 1=fatal, 2=error, 3=info, 4=debug, 5=trace)\n");
	fprintf(stderr, "       -l      is the lcd to use (default /dev/lcd0)\n");
	fprintf(stderr, "       -r      is the reference signal level for 0dB on the meter\n");
	fprintf(stderr, "       -s      is the [optional] name given the jack server when it was started\n");
	fprintf(stderr, "       -n      changes mode to output meter level as number in decibels\n");
	fprintf(stderr, "       -c      the name of the fifo (default /run/jack_meter)\n");
	fprintf(stderr, "       <port>  the port(s) to monitor (multiple ports are mixed)\n");
	exit(1);
}

void write_buffer_to_lcd( const char * const display_buffer, int len ) {
    int expected = len*sizeof(char);
    debug( 4, "LCD: %d (%d) characters\n", len, expected );
    int written = write( lcd, display_buffer, expected );
    if (written != expected ) {
        debug( 2, "*** only wrote %d of %d bytes\n", written, expected);
    }
    debug( 4, "LCD: written %d\n", written );
}

char* configure_buffer( char* display_buffer, char row ) {
    // set the standard prefix for the display
    display_buffer[0] = (char)0x1b;
    display_buffer[1] ='[';
    display_buffer[2] = row;
    display_buffer[3] = ';';
    display_buffer[4] = '0';
    display_buffer[5] = 'H';
    return &display_buffer[6];
}

void clear_display() {
    char display_buffer[DISPLAY_WIDTH];
    configure_buffer( display_buffer, '2' );
    write_buffer_to_lcd( display_buffer, DISPLAY_WIDTH );
    configure_buffer( display_buffer, '3' );
    write_buffer_to_lcd( display_buffer, DISPLAY_WIDTH );
    configure_buffer( display_buffer, '4' );
    write_buffer_to_lcd( display_buffer, DISPLAY_WIDTH );
}

void display_meter( struct channel_info_t *info )
{
    char display_buffer[DISPLAY_WIDTH];
    char* display_text = configure_buffer( display_buffer, '3'+info->channel );
    debug( 4, "Processing db=%d for channel %d\n", info->db, info->channel );
	int size = iec_scale( info->db, CONSOLE_WIDTH );
	debug(4, "size %d\n", size );
	    if (size > info->dpeak) {
	        info->dpeak = size;
	        info->dtime = 0;
    } else if (info->dtime++ > decay_len) {
        info->dpeak = size;
    }
    debug( 5, "display_buffer=%p\ndisplay_text=%p\ndpeak=%i\nsize=%i\n", display_buffer, display_text, info->dpeak, size );

    memset( display_text, ' ', CONSOLE_WIDTH*sizeof(char));
    memset( display_text, meter_char, size*sizeof(char) );
    display_text[info->dpeak]=peak_char;

    // write the line
    write_buffer_to_lcd( display_buffer, DISPLAY_WIDTH );
}

void display_db( struct channel_info_t const *info )
{
    debug( 4, "Processing db=%d for channel %d\n", info->db, info->channel );
    char display_buffer[DISPLAY_WIDTH];
    char* display_text = configure_buffer( display_buffer, '3'+info->channel );
    memset( display_text, ' ', CONSOLE_WIDTH*sizeof(char));
    sprintf( display_text, "%1.1f", info->db);

    debug( 5, "Disp: %s\n", display_text );
    // write the line
    write_buffer_to_lcd( display_buffer, DISPLAY_WIDTH );
}

static int  increment_xrun(void *arg) {
    debug( 2, "XRUN\n" );
    char display_buffer[DISPLAY_WIDTH];
    char* display_text = configure_buffer( display_buffer, '2' );
    xrun_count++;
    int size = sprintf( display_text, "Xrun: %d", xrun_count);
    write_buffer_to_lcd( display_buffer, DISPLAY_SIZE( size ) );

    return 0;
}

void initialise_display() {

    char display_buffer[DISPLAY_WIDTH];
    // clear lines 2, 3, and 4t
    memset( display_buffer, 0, DISPLAY_WIDTH*sizeof(char) );
    int size = sprintf( display_buffer, "%c[2;0H%c[0J", (char)0x1b, (char)0x1b );
    write_buffer_to_lcd( display_buffer, size );

    // display the xrun
    xrun_count = -1;
    increment_xrun( NULL );

}

char* copy_malloc( char* s ) {
    return strcpy ((char *) malloc (sizeof (char) * (strlen(s)+1)), s);
}

void free_copy( char * s ) {
    if ( s ) {
        free( s );
    }
}

char parse_char( char * s ) {
    int len = strlen(s);
    if (len == 0) {
        debug(2, "No parameter string provided\n");
    }
    if (s[0] == '0') {
        if (len == 1) {
            return s[0];
        }
        if (len != 4 ) {
            debug(2, "Exactly 2 hex characters must be provided in the form 0xNN for an escaped character (%s provided)\n", s );
            return s[0];
        } else {
            unsigned n;
            sscanf( s, "%x", &n );
            debug( 5, "Parsed %d (0x%x) from %s\n", n, n, s );
            return (char)n;
        }
    }
    return s[0];
}

void remove_fifo( const char * name ) {
    if (name != 0) {
        if (access( name, F_OK ) == 0 ) {
            remove( name );
        }
    }
}

FILE* make_fifo( const char * name ) {
    remove_fifo( fifo_name );
    fifo_name = copy_malloc( name );
    remove_fifo( fifo_name );
    debug( 3, "Creating fifo %s\n", fifo_name);
    umask( 0 );
    mkfifo( fifo_name, 0666);
    int fd1 = open( fifo_name, O_RDONLY | O_NONBLOCK );
    FILE *f =  fdopen( fd1, "r" );
    setbuf(f, NULL);
    return f;
}

/* Close down JACK when exiting */
static void cleanup()
{
    const char **all_ports;
    unsigned int i;
    unsigned int channel;
    debug( 2, "cleanup()\n");

    for (channel = 0; channel < MAX_CHANNELS; channel++) {
        if (channel_info[channel].input_port != NULL ) {

            all_ports = jack_port_get_all_connections(client, channel_info[channel].input_port);

            for (i=0; all_ports && all_ports[i]; i++) {
                jack_disconnect(client, all_ports[i], jack_port_name(channel_info[channel].input_port));
            }
        }
    }
    /* Leave the jack graph */
    jack_client_close(client);
    remove_fifo( fifo_name );
    free_copy( fifo_name );
    free_copy( server_name );
    free_copy( lcd_device );

}
int main(int argc, char *argv[])
{
    jack_status_t status;
	int running = 1;
	float ref_lev;
	int decibels_mode = 0;
	int rate = 8;
	int opt;

	// clear channel info
	memset( channel_info, 0, (MAX_CHANNELS)*sizeof(struct channel_info_t));

	// Make STDOUT unbuffered
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	while ((opt = getopt(argc, argv, "d:p:m:s:f:r:l:c:nhv")) != -1) {
		switch (opt) {
		    case 'p':
		        peak_char = parse_char( optarg );
                debug( 3, "Setting peak char %x\n", peak_char);
		        break;
		    case 'm':
		        meter_char = parse_char( optarg );
		        debug( 3, "Setting meter char %x\n", meter_char);
		        break;
		    case 'd':
		        debug_level = atoi(optarg);
                debug( 1, "Setting debug level %i\n",debug_level);
		        break;
			case 's':
				server_name = copy_malloc( optarg );
				options |= JackServerName;
                debug( 3, "Setting server name %s\n", server_name );
				break;
			case 'l':
			    lcd_device = copy_malloc( optarg );
                debug( 3, "Setting lcd_device %s\n", server_name );
                break;
			case 'r':
				ref_lev = atof(optarg);
				debug( 3, "Reference level: %.1fdB\n", ref_lev);
				bias = powf(10.0f, ref_lev * -0.05f);
				break;
			case 'f':
				rate = atoi(optarg);
				debug( 3,"Updates per second: %d\n", rate);
				break;
			case 'n':
				decibels_mode = 1;
				break;
			case 'c':
			    fifo = make_fifo( optarg );
			    break;
			case 'h':
			case 'v':
			default:
				/* Show usage/version information */
				usage( argv[0] );
				break;
		}
	}

	if (!fifo) {
	    fifo = make_fifo( DEFAULT_FIFO_NAME );
	}
	if (!fifo) {
	    debug( 1, "Unable to open FIFO");
	    exit(1);
	}

	// ensure we have a device
	if (!lcd_device) {
        lcd_device = copy_malloc( DEFAULT_DEVICE );
	}
	debug( 3, "Using LCD %s\n", lcd_device );
	lcd = open( lcd_device, O_WRONLY);
	debug( 3, "LCD %s opened as %d\n", lcd_device, lcd );

    // ensure the entire display buffer has been cleared
    initialise_display();

	// Register with Jack
	if ((client = jack_client_open("meter", options, &status, server_name)) == 0) {
		debug( 1, "Failed to start jack client: %d\n", status);
		exit(1);
	}
	debug( 3,"Registering as '%s'.\n", jack_get_client_name( client ) );

	// Create our input ports
	unsigned int channel;
	for (channel = 0; channel < MAX_CHANNELS; channel++)
	{
	    char port_name[10];
	    sprintf( port_name, "in%d", channel );
	    debug( 4, "Registering port '%s' on channel %d.\n", port_name, channel );
        if (!(channel_info[channel].input_port = jack_port_register(client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
            debug( 1, "Cannot register input port 'meter:%s'.\n", port_name );
            exit(1);
        }
	}
	
	// Register the cleanup function to be called when program exits
	atexit( cleanup );

	// register the xrun callback
	jack_set_xrun_callback( client, increment_xrun, 0 );
	// Register the peak signal callback
	jack_set_process_callback(client, process_peak, 0);

	if (jack_activate(client)) {
		debug( 1, "Cannot activate client.\n");
		exit(1);
	}


	// Connect our port to specified port(s)
	if (argc > optind) {

	    channels=0;
		while (argc > optind && channels<MAX_CHANNELS) {
			connect_port( client, argv[ optind ], channels );
			optind++;
			channel_info[channels].channel = channels;
			channels++;
		}
	} else {
		debug( 2, "Meter is not connected to a port.\n");
	}

	// Calculate the decay length (should be 1600ms)
	decay_len = (int)(1.6f / (1.0f/rate));

	struct channel_info_t *info;
	int cmd;
	while (running) {

	    // check for state change
	    clearerr(fifo);
	    cmd = fgetc( fifo );
	    int err = ferror(fifo);
	    if (err) {
	        debug( 3, "Read error on fifo: %d", err );
	    } else {
	        if (cmd == '0') {
	            displaying = 0;
	            clear_display();
	        } else if (cmd == '1' ) {
	            displaying = 1;
	        } else if (cmd == 'x' ) {
	            running=0;
	            break;
	        }
	    }

	    debug( 4, "update %d displays\n", channels );
	    for  (channel = 0; channel < channels; channel++ )
	    {
	        info = &channel_info[channel];
	        info->last_peak = info->peak;
	        channel_info[channel].peak = 0.0f;
	        info->db = 20.0f * log10f(info->last_peak * bias);
	        if (displaying) {
                if (decibels_mode==1) {
                    display_db( info );
                } else {
                    display_meter( info );
                }
	        }
	    }
        fsleep( 1.0f/rate );
        debug( 4, "WOKE UP\n" );
	}
	cleanup();
	return 0;
}

