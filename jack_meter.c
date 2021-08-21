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
#include <math.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include <jack/jack.h>
#include <getopt.h>
#include "config.h"


float bias = 1.0f;
//float peak = 0.0f;

//int dpeak = 0;
//int dtime = 0;
int decay_len;
char *server_name = NULL;

jack_client_t *client = NULL;
jack_options_t options = JackNoStartServer;

/* constants for lcd access */
#define DEFAULT_DEVICE "/dev/lcd0"
#define CONSOLE_WIDTH 20
#define DISPLAY_WIDTH (CONSOLE_WIDTH+6)
char display_buffer[ DISPLAY_WIDTH ];
int xrun_count = 0;
char* display_row = NULL;
char* display_text = NULL;
char* lcd_device = NULL;
char peak_char='I';
char meter_char='#';
int lcd;

/* struct to handle 2 different displays (channels)*/
#define MAX_CHANNELS 2
unsigned int channels = 0;
struct channel_t {
    int dpeak;
    int dtime;
    float peak;
    jack_port_t *input_port;
} channel_info[MAX_CHANNELS];


/* Read and reset the recent peak sample */
static float read_peak(int channel)
{
	float tmp = channel_info[channel].peak;
	channel_info[channel].peak = 0.0f;

	return tmp;
}


/* Callback called by JACK when audio is available.
   Stores value of peak sample */
static int process_peak(jack_nframes_t nframes, void *arg)
{
	jack_default_audio_sample_t *in;
	unsigned int channel;
	unsigned int i;

	for (channel = 0; channel < channels; channel++) {

        /* just incase the port isn't registered yet */
        if (channel_info[channel].input_port == NULL) {
            break;
        }


        /* get the audio samples, and find the peak sample */
        in = (jack_default_audio_sample_t *) jack_port_get_buffer(channel_info[channel].input_port, nframes);
        for (i = 0; i < nframes; i++) {
            const float s = fabs(in[i]);
            if (s > channel_info[channel].peak) {
                channel_info[channel].peak = s;
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


/* Close down JACK when exiting */
static void cleanup()
{
	const char **all_ports;
	unsigned int i;
	unsigned int channel;
	fprintf(stderr,"cleanup()\n");

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

}


/* Connect the chosen port to ours */
static void connect_port(jack_client_t *client, char *port_name, unsigned int channel)
{
	jack_port_t *port;

	// Get the port we are connecting to
	port = jack_port_by_name(client, port_name);
	if (port == NULL) {
		fprintf(stderr, "Can't find port '%s'\n", port_name);
		exit(1);
	}

	// Connect the port to our input port
	fprintf(stderr,"Connecting '%s' to '%s'...\n", jack_port_name(port), jack_port_name(channel_info[channel].input_port));
	if (jack_connect(client, jack_port_name(port), jack_port_name(channel_info[channel].input_port))) {
		fprintf(stderr, "Cannot connect port '%s' to '%s'\n", jack_port_name(port), jack_port_name(channel_info[channel].input_port));
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
	fprintf(stderr, "       -l      is the lcd to use (default /dev/lcd0)\n");
	fprintf(stderr, "       -r      is the reference signal level for 0dB on the meter\n");
	fprintf(stderr, "       -s      is the [optional] name given the jack server when it was started\n");
	fprintf(stderr, "       -n      changes mode to output meter level as number in decibels\n");
	fprintf(stderr, "       <port>  the port(s) to monitor (multiple ports are mixed)\n");
	exit(1);
}

void write_buffer_to_lcd( int len ) {
    int expected = len*sizeof(char);
    int written = write( lcd, display_buffer, expected );
    if (written != expected ) {
        fprintf( stderr, "*** only wrote %d of %d bytes\n", written, expected);
    }
}
void display_meter( unsigned int channel, int db )
{
	int size = iec_scale( db, CONSOLE_WIDTH );
    if (size > channel_info[channel].dpeak) {
        channel_info[channel].dpeak = size;
        channel_info[channel].dtime = 0;
    } else if (channel_info[channel].dtime++ > decay_len) {
        channel_info[channel].dpeak = size;
    }

    memset( display_text, ' ', CONSOLE_WIDTH*sizeof(char));
    memset( display_text, meter_char, size*sizeof(char) );
    *display_row = (char)'3'+channel;
    display_text[channel_info[channel].dpeak]=peak_char;

    // write the line
    write_buffer_to_lcd( DISPLAY_WIDTH );
}

void display_db( unsigned int channel, float db )
{
    memset( display_text, ' ', CONSOLE_WIDTH*sizeof(char));
    int size = sprintf( display_text, "%1.1f", db);
    display_text[size] = ' ';
    *display_row = (char)'3'+channel;
    // write the line
    write_buffer_to_lcd( DISPLAY_WIDTH );
}

static int  increment_xrun(void *arg) {
    xrun_count++;
    memset( display_text, ' ', CONSOLE_WIDTH*sizeof(char) );
    int size = sprintf( display_text, "Xrun: %d", xrun_count);
    display_text[size] = ' ';
    *display_row = '2';
    write_buffer_to_lcd( DISPLAY_WIDTH );

    return 0;
}

void initialise_display() {

    // clear lines 2, 3, and 4t
    memset( display_buffer, 0, DISPLAY_WIDTH*sizeof(char) );
    int size = sprintf( display_buffer, "%c[2;0H%c[0J", (char)0x1b, (char)0x1b );
    write_buffer_to_lcd( size );


    // set the standard prefix for the display
    display_buffer[0] = (char)0x1b;
    display_buffer[1] ='[';
    display_buffer[2] = '3';
    display_buffer[3] = ';';
    display_buffer[4] = '0';
    display_buffer[5] = 'H';
    display_row = &display_buffer[2];
    display_text = &display_buffer[6];

    // display the xrun
    xrun_count = -1;
    increment_xrun( NULL );

}

char* copy_malloc( char* s ) {
    return strcpy ((char *) malloc (sizeof (char) * (strlen(s)+1)), s);
}

int main(int argc, char *argv[])
{

	jack_status_t status;
	int running = 1;
	float ref_lev;
	int decibels_mode = 0;
	int rate = 8;
	int opt;

	// ensure the entire display buffer has been cleared
	initialise_display();

	// clear channel info
	memset( channel_info, 0, (MAX_CHANNELS)*sizeof(struct channel_t));

	// Make STDOUT unbuffered
	setbuf(stdout, NULL);

	while ((opt = getopt(argc, argv, "s:w:f:r:nhv")) != -1) {
		switch (opt) {
			case 's':
				server_name = copy_malloc( optarg );
				options |= JackServerName;
				break;
			case 'l':
			    lcd_device = copy_malloc( optarg );
                break;
			case 'r':
				ref_lev = atof(optarg);
				fprintf(stderr,"Reference level: %.1fdB\n", ref_lev);
				bias = powf(10.0f, ref_lev * -0.05f);
				break;
			case 'f':
				rate = atoi(optarg);
				fprintf(stderr,"Updates per second: %d\n", rate);
				break;
			case 'n':
				decibels_mode = 1;
				break;
			case 'h':
			case 'v':
			default:
				/* Show usage/version information */
				usage( argv[0] );
				break;
		}
	}

	// ensure we have a device
	if (!lcd_device) {
        lcd_device = copy_malloc( DEFAULT_DEVICE );
	}
	fprintf(stderr, "Using LCD %s\n", lcd_device );
	lcd = open( lcd_device, O_WRONLY);
	fprintf(stderr, "LCD %s opened as %d\n", lcd_device, lcd );


	// Register with Jack
	if ((client = jack_client_open("meter", options, &status, server_name)) == 0) {
		fprintf(stderr, "Failed to start jack client: %d\n", status);
		exit(1);
	}
	fprintf(stderr,"Registering as '%s'.\n", jack_get_client_name( client ) );

	// Create our input ports
	unsigned int channel;
	for (channel = 0; channel < MAX_CHANNELS; channel++)
	{
	    char port_name[10];
	    sprintf( port_name, "in%d", channel );

        if (!(channel_info[channel].input_port = jack_port_register(client, port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
            fprintf(stderr, "Cannot register input port 'meter:%s'.\n", port_name );
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
		fprintf(stderr, "Cannot activate client.\n");
		exit(1);
	}


	// Connect our port to specified port(s)
	if (argc > optind) {

	    channels=0;
		while (argc > optind && channel<MAX_CHANNELS) {
			connect_port( client, argv[ optind ], channels );
			optind++;
			channels++;
		}
	} else {
		fprintf(stderr,"Meter is not connected to a port.\n");
	}

	// Calculate the decay length (should be 1600ms)
	decay_len = (int)(1.6f / (1.0f/rate));

	while (running) {

	    for  (channel = 0; channel < channels; channels++ )
	    {
            float db = 20.0f * log10f(read_peak(channel) * bias);

            if (decibels_mode==1) {
                display_db( channel, db );
            } else {
                display_meter( channel, db );
            }
	    }
        fsleep( 1.0f/rate );
	}

	return 0;
}

