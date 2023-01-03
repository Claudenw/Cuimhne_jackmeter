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

int decay_len;
char *server_name = NULL;

jack_client_t *client = NULL;
jack_options_t options = JackNoStartServer;

/* constants for lcd access */
#define DEFAULT_DEVICE "/dev/lcd0"
#define CONSOLE_WIDTH 20
#define DISPLAY_WIDTH (CONSOLE_WIDTH+6)
#define DISPLAY_SIZE(s) (s+6)

#define ESC (char)0x1b
#define CLEAR_TO_END '0'
#define CLEAR_TO_BEGINNING '1'
#define CLEAR_ALL '2'
#define CLEAR_LINE "%c[%cK"
#define CLEAR_SCREEN "%c[%cJ"

#define CMD_NO_DISPLAY '0'
#define CMD_ONE_DISPLAY '1'
#define CMD_TWO_DISPLAY '2'
#define CMD_STOP_RECORDING 'r'
#define CMD_START_RECORDING 'R'
#define CMD_EXIT 'x'
#define DEFAULT_FIFO_NAME "/run/jack_meter"
char *fifo_name = NULL;
FILE *fifo = NULL;

char *lcd_device = NULL;
char peak_char = 'I';
char meter_char = '#';
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

/*
 * Display handling
 */
struct display_info_t {
	int recording;
	int xrun_count;
	int displaying;
	time_t start_time;
	time_t elapsed_seconds;
	int channels_installed;
	int channels_displaying;
	int decibels_mode;
	int update_rate;
	float bias;
	char xrun_len;
};

/* DEBUG */

static unsigned int debug_level = 3;

static void debug(unsigned int level, const char *fmt, ...) {
	va_list argp;
	if (level <= debug_level) {
		va_start(argp, fmt);
		vfprintf(stderr, fmt, argp);
		va_end(argp);
	}
}

/* Callback called by JACK when audio is available.
 Stores value of peak sample */
static int process_peak(jack_nframes_t nframes, void *arg) {
	jack_default_audio_sample_t *in;
	unsigned int channel;
	unsigned int i;
	struct channel_info_t *info;
	for (channel = 0; channel < channels; channel++) {
		info = &channel_info[channel];
		/* just incase the port isn't registered yet */
		if (info->input_port == NULL) {
			debug(2, "Channel %d is not enabled\n", channel);
		} else {
			/* get the audio samples, and find the peak sample */
			in = (jack_default_audio_sample_t*) jack_port_get_buffer(
					info->input_port, nframes);
			for (i = 0; i < nframes; i++) {
				const float s = fabs(in[i]);
				if (s > info->peak) {
					debug(4, "Setting channel %d peak %f\n", channel, s);
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

	return (int) ((def / 100.0f) * ((float) size));
}

/* Connect the chosen port to ours */
static void connect_port(jack_client_t *client, char *port_name,
		unsigned int channel) {
	jack_port_t *port;

	// Get the port we are connecting to
	port = jack_port_by_name(client, port_name);
	if (port == NULL) {
		debug(1, "Can't find port '%s'\n", port_name);
		exit(1);
	}
	const char *fq_port_name = jack_port_name(port);
	const char *fq_channel_name = jack_port_name(
			channel_info[channel].input_port);
	// Connect the port to our input port
	debug(4, "Connecting '%s' to '%s' on channel %d\n", fq_port_name,
			fq_channel_name, channel);
	if (jack_connect(client, fq_port_name, fq_channel_name)) {
		debug(1, "Cannot connect '%s' to '%s' on channel %d\n", fq_port_name,
				fq_channel_name, channel);
		exit(1);
	}
}

/* Sleep for a fraction of a second */
static int fsleep(float secs) {

//#ifdef HAVE_USLEEP
	return usleep(secs * 1000000);
//#endif
}

/* Display how to use this program */
static int usage(const char *progname) {
	fprintf(stderr, "jackmeter version %s\n\n", VERSION);
	fprintf(stderr,
			"Usage %s [-f freqency] [-r ref-level] [-s servername] [-n] [<port>, ...]\n\n",
			progname);
	fprintf(stderr,
			"where  -f      is how often to update the meter per second [8]\n");
	fprintf(stderr,
			"       -d      is the debug level (0 = silent, 1=fatal, 2=error, 3=info, 4=debug, 5=trace)\n");
	fprintf(stderr, "       -l      is the lcd to use (default /dev/lcd0)\n");
	fprintf(stderr,
			"       -r      is the reference signal level for 0dB on the meter\n");
	fprintf(stderr,
			"       -s      is the [optional] name given the jack server when it was started\n");
	fprintf(stderr,
			"       -n      changes mode to output meter level as number in decibels\n");
	fprintf(stderr,
			"       -c      the name of the fifo (default /run/jack_meter)\n");
	fprintf(stderr,
			"       <port>  the port(s) to monitor (multiple ports are mixed)\n");
	exit(1);
}

void write_buffer_to_lcd(const char *const display_buffer, int len) {
	int expected = len * sizeof(char);
	debug(5, "LCD: %d (%d) characters\n", len, expected);
	int written = write(lcd, display_buffer, expected);
	if (written != expected) {
		debug(2, "*** only wrote %d of %d bytes\n", written, expected);
	}
	debug(4, "LCD: %d characters written\n", written);
}

/**
 * add codes to the front of the buffer to position the cursor to the first position on the line.
 * @param display_buffer the buffer to write 6 chars into.
 * @param row the row to position to
 */
char* configure_buffer(char *display_buffer, char row) {
	display_buffer[0] = ESC;
	display_buffer[1] = '[';
	display_buffer[2] = row;
	display_buffer[3] = ';';
	display_buffer[4] = '0';
	display_buffer[5] = 'H';
	return &display_buffer[6];
}

/**
 * set the column in the prefix used in the configure_buffer.  Position can be set from 0 to 9
 */
void set_column_number(char *display_buffer, char column) {
	display_buffer[4] = column;
}

void clear_display(struct display_info_t *display_info) {
	if (display_info->channels_displaying > 0) {
		char display_buffer[DISPLAY_WIDTH];
		char *text_buffer = configure_buffer(display_buffer, '3');
		int size = 0;
		if (display_info->channels_displaying == 2) {
			size = sprintf(text_buffer, CLEAR_SCREEN, ESC, CLEAR_TO_END);
		} else {
			size = sprintf(text_buffer, CLEAR_LINE, ESC, CLEAR_ALL);
		}
		write_buffer_to_lcd(display_buffer, DISPLAY_SIZE(size));
	}
}

void display_time(struct display_info_t *display_info) {
	uint minutes = display_info->elapsed_seconds / 60;
	uint seconds = display_info->elapsed_seconds % 60;

	char display_buffer[DISPLAY_WIDTH];
	char time_pos = '1' + display_info->xrun_len;
	char *text_buffer = configure_buffer(display_buffer, '2');
	set_column_number(display_buffer, (char) time_pos);
	int size = sprintf(text_buffer, "  T:%02i:%02i", minutes, seconds);
	write_buffer_to_lcd(display_buffer, DISPLAY_SIZE(size));
}

void display_meter(struct channel_info_t *info) {
	char display_buffer[DISPLAY_WIDTH];
	char *display_text = configure_buffer(display_buffer, '3' + info->channel);
	debug(4, "Processing db=%d for channel %d\n", info->db, info->channel);
	int size = iec_scale(info->db, CONSOLE_WIDTH);
	debug(4, "size %d\n", size);
	if (size > info->dpeak) {
		info->dpeak = size;
		info->dtime = 0;
	} else if (info->dtime++ > decay_len) {
		info->dpeak = size;
	}
	debug(5, "display_buffer=%p\ndisplay_text=%p\ndpeak=%i\nsize=%i\n",
			display_buffer, display_text, info->dpeak, size);

	memset(display_text, ' ', CONSOLE_WIDTH * sizeof(char));
	memset(display_text, meter_char, size * sizeof(char));
	display_text[info->dpeak] = peak_char;

	// write the line
	write_buffer_to_lcd(display_buffer, DISPLAY_WIDTH);
}

void display_db(struct channel_info_t const *info) {
	debug(4, "Processing db=%d for channel %d\n", info->db, info->channel);
	char display_buffer[DISPLAY_WIDTH];
	char *display_text = configure_buffer(display_buffer, '3' + info->channel);
	memset(display_text, ' ', CONSOLE_WIDTH * sizeof(char));
	sprintf(display_text, "%1.1f", info->db);

	debug(5, "Disp: %s\n", display_text);
	// write the line
	write_buffer_to_lcd(display_buffer, DISPLAY_WIDTH);
}

void display_xrun(struct display_info_t *display_info) {
	if (display_info->channels_displaying && display_info->recording) {
		char display_buffer[DISPLAY_WIDTH];
		char *display_text = configure_buffer(display_buffer, '2');
		display_info->xrun_len = (char) sprintf(display_text, "X: %d",
				display_info->xrun_count);
		write_buffer_to_lcd(display_buffer,
				DISPLAY_SIZE(display_info->xrun_len));
	}
}

static int increment_xrun(void *arg) {
	struct display_info_t *display_info = (struct display_info_t*) arg;
	if (display_info->xrun_count >= 0) {
		debug(4, "XRUN\n");
	}
	display_info->xrun_count++;
	display_xrun(display_info);
	return 0;
}

char* copy_malloc(const char *s) {
	return strcpy((char*) malloc(sizeof(char) * (strlen(s) + 1)), s);
}

void free_copy(char *s) {
	if (s) {
		free(s);
	}
}

char parse_char(char *s) {
	int len = strlen(s);
	if (len == 0) {
		debug(2, "No parameter string provided\n");
	}
	if (s[0] == '0') {
		if (len == 1) {
			return s[0];
		}
		if (len != 4) {
			debug(2,
					"Exactly 2 hex characters must be provided in the form 0xNN for an escaped character (%s provided)\n",
					s);
			return s[0];
		} else {
			unsigned n;
			sscanf(s, "%x", &n);
			debug(5, "Parsed %d (0x%x) from %s\n", n, n, s);
			return (char) n;
		}
	}
	return s[0];
}

void remove_fifo(const char *name) {
	if (name != 0) {
		if (access(name, F_OK) == 0) {
			remove(name);
		}
	}
}

FILE* make_fifo(const char *name) {
	remove_fifo(fifo_name);
	fifo_name = copy_malloc(name);
	remove_fifo(fifo_name);
	debug(3, "Creating fifo %s\n", fifo_name);
	umask(0);
	mkfifo(fifo_name, 0666);
	int fd1 = open(fifo_name, O_RDONLY | O_NONBLOCK);
	FILE *f = fdopen(fd1, "r");
	setbuf(f, NULL);
	return f;
}

/* Close down JACK when exiting */
static void cleanup() {
	const char **all_ports;
	unsigned int i;
	unsigned int channel;
	debug(2, "cleanup()\n");

	for (channel = 0; channel < MAX_CHANNELS; channel++) {
		if (channel_info[channel].input_port != NULL) {

			all_ports = jack_port_get_all_connections(client,
					channel_info[channel].input_port);

			for (i = 0; all_ports && all_ports[i]; i++) {
				jack_disconnect(client, all_ports[i],
						jack_port_name(channel_info[channel].input_port));
			}
		}
	}
	/* Leave the jack graph */
	jack_client_close(client);
	remove_fifo(fifo_name);
	free_copy(fifo_name);
	free_copy(server_name);
	free_copy(lcd_device);
}

void clear_recording_status() {
	char display_buffer[DISPLAY_WIDTH];
	char *text_buffer = configure_buffer(display_buffer, '2');
	int size = sprintf(text_buffer, CLEAR_LINE, ESC, CLEAR_ALL);
	write_buffer_to_lcd(display_buffer, DISPLAY_SIZE(size));
}

int check_cmd(struct display_info_t *display_info) {
	// check for state change
	clearerr(fifo);
	char cmd = fgetc(fifo);
	int err = ferror(fifo);
	if (err) {
		debug(3, "Read error on fifo: %d", err);
		return 1;
	}
	switch (cmd) {
	case CMD_NO_DISPLAY:
		clear_display(display_info);
		display_info->channels_displaying = 0;
		break;
	case CMD_ONE_DISPLAY:
		clear_display(display_info);
		display_info->channels_displaying = 1;
		break;
	case CMD_TWO_DISPLAY:
		clear_display(display_info);
		display_info->channels_displaying = 2;
		break;
	case CMD_STOP_RECORDING:
		display_info->recording = 0;
		clear_recording_status();
		break;
	case CMD_START_RECORDING:
		display_info->recording = 1;
		clear_recording_status();
		time(&display_info->start_time);
		display_info->elapsed_seconds = 0;
		display_info->xrun_count = 0;
		display_xrun(display_info);
		display_time(display_info);
		break;
	case CMD_EXIT: // exit program
		if (display_info->recording) {
			clear_recording_status();
		}
		clear_display (display_info);
		return 0;
	}
	return 1;
}

void update_display(struct display_info_t *display_info) {
	if (display_info->channels_displaying) {
		int channel;
		struct channel_info_t *info;
		debug(4, "update %d displays\n", channels);
		for (channel = 0; channel < display_info->channels_displaying;
				channel++) {
			info = &channel_info[channel];
			info->last_peak = info->peak;
			channel_info[channel].peak = 0.0f;
			info->db = 20.0f * log10f(info->last_peak * display_info->bias);

			if (display_info->decibels_mode == 1) {
				display_db(info);
			} else {
				display_meter(info);
			}
		}
		if (display_info->recording) {
			time_t seconds = time(NULL) - display_info->start_time;
			if (seconds != display_info->elapsed_seconds) {
				display_info->elapsed_seconds = seconds;
				display_time(display_info);
			}
		}
	}
}

int main(int argc, char *argv[]) {
	jack_status_t status;
	float ref_lev;
	int opt;

	struct display_info_t display_info;
	memset(&display_info, 0, sizeof(struct display_info_t));
	display_info.update_rate = 8;
	display_info.bias = 1.0f;

	// clear channel info
	memset(channel_info, 0, (MAX_CHANNELS) * sizeof(struct channel_info_t));

	// Make STDOUT unbuffered
	setbuf(stdout, NULL);
	setbuf(stderr, NULL);

	while ((opt = getopt(argc, argv, "d:p:m:s:f:r:l:c:nhv")) != -1) {
		switch (opt) {
		case 'p':
			peak_char = parse_char(optarg);
			debug(3, "Setting peak char %x\n", peak_char);
			break;
		case 'm':
			meter_char = parse_char(optarg);
			debug(3, "Setting meter char %x\n", meter_char);
			break;
		case 'd':
			debug_level = atoi(optarg);
			debug(1, "Setting debug level %i\n", debug_level);
			break;
		case 's':
			server_name = copy_malloc(optarg);
			options |= JackServerName;
			debug(3, "Setting server name %s\n", server_name);
			break;
		case 'l':
			lcd_device = copy_malloc(optarg);
			debug(3, "Setting lcd_device %s\n", lcd_device);
			break;
		case 'r':
			ref_lev = atof(optarg);
			debug(3, "Reference level: %.1fdB\n", ref_lev);
			display_info.bias = powf(10.0f, ref_lev * -0.05f);
			break;
		case 'f':
			display_info.update_rate = atoi(optarg);
			debug(3, "Updates per second: %d\n", display_info.update_rate);
			break;
		case 'n':
			debug(3, "Using decibels mode\n");
			display_info.decibels_mode = 1;
			break;
		case 'c':
			debug(3, "Using fifo channel: %s\n", optarg);
			fifo = make_fifo(optarg);
			break;
		case 'h':
		case 'v':
		default:
			/* Show usage/version information */
			usage(argv[0]);
			break;
		}
	}

	if (!fifo) {
		fifo = make_fifo( DEFAULT_FIFO_NAME);
	}
	if (!fifo) {
		debug(1, "Unable to open FIFO");
		exit(1);
	}

	// ensure we have a device
	if (!lcd_device) {
		lcd_device = copy_malloc( DEFAULT_DEVICE);
	}
	debug(3, "Using LCD %s\n", lcd_device);
	lcd = open(lcd_device, O_WRONLY);
	debug(3, "LCD %s opened as %d\n", lcd_device, lcd);

	// ensure the entire display buffer has been cleared
	clear_display(&display_info);

	// Register with Jack
	if ((client = jack_client_open("meter", options, &status, server_name))	== 0) {
		debug(1, "Failed to start jack client: %d\n", status);
		exit(1);
	}
	debug(3, "Registering as '%s'.\n", jack_get_client_name(client));

	// Create our input ports
	unsigned int channel;
	for (channel = 0; channel < MAX_CHANNELS; channel++) {
		char port_name[10];
		sprintf(port_name, "in_%d", channel);
		debug(4, "Registering port '%s' on channel %d.\n", port_name, channel);
		if (!(channel_info[channel].input_port = jack_port_register(client,
				port_name, JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0))) {
			debug(1, "Cannot register input port 'meter:%s'.\n", port_name);
			exit(1);
		}
	}

	display_info.channels_installed = channels;

	// Register the cleanup function to be called when program exits
	atexit(cleanup);

	// register the xrun callback
	jack_set_xrun_callback(client, increment_xrun, &display_info);
	// Register the peak signal callback
	jack_set_process_callback(client, process_peak, 0);

	if (jack_activate(client)) {
		debug(1, "Cannot activate client.\n");
		exit(1);
	}

	// Connect our port to specified port(s)
	if (argc > optind) {

		channels = 0;
		while (argc > optind && channels < MAX_CHANNELS) {
			connect_port(client, argv[optind], channels);
			optind++;
			channel_info[channels].channel = channels;
			channels++;
		}
	} else {
		debug(2, "Meter is not connected to a port.\n");
	}

	// Calculate the decay length (should be 1600ms)
	decay_len = (int) (1.6f / (1.0f / display_info.update_rate));

	while (check_cmd(&display_info)) {
		update_display(&display_info);
		fsleep(1.0f / display_info.update_rate);
		debug(4, "WOKE UP\n");
	}
	clear_display(&display_info);
	return 0;
}

