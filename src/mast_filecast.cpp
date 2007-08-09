/*
 *  MAST: Multicast Audio Streaming Toolkit
 *
 *  Copyright (C) 2003-2007 Nicholas J. Humfrey <njh@ecs.soton.ac.uk>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  $Id$
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <string.h>
#include <getopt.h>

#include <ortp/ortp.h>
#include <sndfile.h>

#include "MastAudioBuffer.h"
#include "MastCodec.h"
#include "MastMimeType.h"
#include "mast.h"


#define MAST_TOOL_NAME	"mast_filecast"


/* Global Variables */
int g_loop_file = FALSE;
int g_payload_size_limit = DEFAULT_PAYLOAD_LIMIT;
char* g_filename = NULL;
MastMimeType *g_mime_type = NULL;




/* 
  format_duration_string() 
  Create human readable duration string from libsndfile info
*/
static char* format_duration_string( SF_INFO *sfinfo )
{
	float seconds;
	int minutes;
	char *string = (char*)malloc( STR_BUF_SIZE );
	
	if (sfinfo->frames==0 || sfinfo->samplerate==0) {
		snprintf( string, STR_BUF_SIZE, "Unknown" );
		return string;
	}
	
	// Calculate the number of minutes and seconds
	seconds = sfinfo->frames / sfinfo->samplerate;
	minutes = (seconds / 60 );
	seconds -= (minutes * 60);

	// Create a string out of it
	snprintf( string, STR_BUF_SIZE, "%imin %1.1fsec", minutes, seconds);

	return string;
}


/*
  fill_input_buffer()
  Make sure input buffer if full of audio  
*/
static size_t fill_input_buffer( SNDFILE *inputfile, MastAudioBuffer* buffer )
{
	size_t frames_read = sf_readf_float( inputfile, buffer->get_write_ptr(), buffer->get_write_space() );

	if (frames_read < 0) {
		MAST_ERROR("Failed to read from file: %s", sf_strerror( inputfile ) );
		return frames_read;
	}

	// Add it on to the buffer usage
	buffer->add_frames( frames_read );

	return frames_read;
}

/* 
  print_file_info() 
  Display information about input and output files
*/
static void print_file_info( SNDFILE *inputfile, SF_INFO *sfinfo )
{
	SF_FORMAT_INFO format_info;
	SF_FORMAT_INFO subformat_info;
	char sndlibver[128];
	char *duration = NULL;
	
	// Get the format
	format_info.format = sfinfo->format & SF_FORMAT_TYPEMASK;
	sf_command (inputfile, SFC_GET_FORMAT_INFO, &format_info, sizeof(format_info)) ;

	// Get the sub-format info
	subformat_info.format = sfinfo->format & SF_FORMAT_SUBMASK;
	sf_command (inputfile, SFC_GET_FORMAT_INFO, &subformat_info, sizeof(subformat_info)) ;

	// Get the version of libsndfile
	sf_command (NULL, SFC_GET_LIB_VERSION, sndlibver, sizeof(sndlibver));

	// Get human readable duration of the input file
	duration = format_duration_string( sfinfo );

	printf( "---------------------------------------------------------\n");
	printf( "%s (http://www.mega-nerd.com/libsndfile/)\n", sndlibver);
	printf( "Input File: %s\n", g_filename );
	printf( "Input Format: %s, %s\n", format_info.name, subformat_info.name );
	printf( "Input Sample Rate: %d Hz\n", sfinfo->samplerate );
	if (sfinfo->channels == 1) printf( "Input Channels: Mono\n" );
	else if (sfinfo->channels == 2) printf( "Input Channels: Stereo\n" );
	else printf( "Input Channels: %d\n", sfinfo->channels );
	printf( "Input Duration: %s\n", duration );
	printf( "---------------------------------------------------------\n");
	
	free( duration );
}


static int usage() {
	
	printf( "Multicast Audio Streaming Toolkit (version %s)\n", PACKAGE_VERSION);
	printf( "%s [options] <address>[/<port>] <filename>\n", MAST_TOOL_NAME);
	printf( "    -s <ssrc>       Source identifier in hex (default is random)\n");
	printf( "    -t <ttl>        Time to live\n");
	printf( "    -p <payload>    The payload mime type to send\n");
	printf( "    -o <name=value> Set codec parameter / option\n");
	printf( "    -z <size>       Set the per-packet payload size\n");
	printf( "    -d <dscp>       DSCP Quality of Service value\n");
	printf( "    -l              Loop the audio file\n");
	
	exit(1);
	
}


static void parse_cmd_line(int argc, char **argv, RtpSession* session)
{
	char* payload_type = DEFAULT_PAYLOAD_TYPE;
	char* remote_address = NULL;
	int remote_port = DEFAULT_RTP_PORT;
	int ch;


	// Parse the options/switches
	while ((ch = getopt(argc, argv, "s:t:p:o:z:d:lh?")) != -1)
	switch (ch) {
		case 's':
			mast_set_session_ssrc( session, optarg );
		break;
		
		case 't':
			if (rtp_session_set_multicast_ttl( session, atoi(optarg) )) {
				MAST_FATAL("Failed to set multicast TTL");
			}
		break;
		
		case 'p':
			payload_type = optarg;
		break;
		
		case 'o':
			g_mime_type->set_param_pair( optarg );
		break;

		case 'z':
			g_payload_size_limit = atoi(optarg);
		break;
		
		case 'd':
			if (rtp_session_set_dscp( session, mast_parse_dscp(optarg) )) {
				MAST_FATAL("Failed to set DSCP value");
			}
		break;
		
		case 'l':
			g_loop_file = TRUE;
		break;
		
		case '?':
		case 'h':
		default:
			usage();
	}


	// Parse the ip address and port
	if (argc > optind) {
		remote_address = argv[optind];
		optind++;
		
		// Look for port in the address
		char* portstr = strchr(remote_address, '/');
		if (portstr && strlen(portstr)>1) {
			*portstr = 0;
			portstr++;
			remote_port = atoi(portstr);
		}
	
	} else {
		MAST_ERROR("missing address/port to send to");
		usage();
	}
	
	// Parse the payload type
	if (g_mime_type->parse( payload_type )) {
		usage();
	}

	// Make sure the port number is even
	if (remote_port%2 == 1) remote_port--;
	
	// Set the remote address/port
	if (rtp_session_set_remote_addr( session, remote_address, remote_port )) {
		MAST_FATAL("Failed to set remote address/port (%s/%d)", remote_address, remote_port);
	} else {
		MAST_INFO( "Remote address: %s/%d", remote_address,  remote_port );
	}
	

	// Get the input file
	if (argc > optind) {
		g_filename = argv[optind];
		optind++;
	} else {
		MAST_ERROR("missing audio input filename");
		usage();
	}

}



int main(int argc, char **argv)
{
	RtpSession* session = NULL;
	PayloadType* pt = NULL;
	SNDFILE* input = NULL;
	SF_INFO sfinfo;
	MastCodec *codec = NULL;
	MastAudioBuffer *input_buffer = NULL;
	u_int8_t *payload_buffer = NULL;
	int frames_per_packet = 0;
	int ts = 0;

	
	// Create an RTP session
	session = mast_init_ortp( MAST_TOOL_NAME, RTP_SESSION_SENDONLY, TRUE );

	// Initialise the MIME type
	g_mime_type = new MastMimeType();

	// Parse the command line arguments 
	// and configure the session
	parse_cmd_line( argc, argv, session );

	
	// Open the input file by filename
	memset( &sfinfo, 0, sizeof(sfinfo) );
	input = sf_open(g_filename, SFM_READ, &sfinfo);
	if (input == NULL) MAST_FATAL("Failed to open input file:\n%s", sf_strerror(NULL));

	
	// Display some information about the input file
	print_file_info( input, &sfinfo );
	
	// Display some information about the chosen payload type
	MAST_INFO( "Sending SSRC: 0x%x", session->snd.ssrc );
	MAST_INFO( "Input Format: %d Hz, %s", sfinfo.samplerate, sfinfo.channels==2 ? "Stereo" : "Mono");
	g_mime_type->print();

	
	// Load the codec
	codec = MastCodec::new_codec( g_mime_type );
	if (codec == NULL) MAST_FATAL("Failed to get initialise codec" );
	MAST_INFO( "Output Format: %d Hz, %s", codec->get_samplerate(), codec->get_channels()==2 ? "Stereo" : "Mono");

	// Work out the payload type to use
	pt = mast_choose_payloadtype( session, codec->get_type(), codec->get_samplerate(), codec->get_channels() );
	if (pt == NULL) MAST_FATAL("Failed to get payload type information from oRTP");

	// Calculate the packet size
	frames_per_packet = codec->frames_per_packet( g_payload_size_limit );
	if (frames_per_packet<=0) MAST_FATAL( "Invalid number of samples per packet" );

	// Create audio buffer
	input_buffer = new MastAudioBuffer( frames_per_packet, codec->get_samplerate(), codec->get_channels() );
	if (input_buffer == NULL) MAST_FATAL("Failed to creare audio input buffer");

	// Allocate memory for the packet buffer
	payload_buffer = (u_int8_t*)malloc( g_payload_size_limit );
	if (payload_buffer == NULL) MAST_FATAL("Failed to allocate memory for payload buffer");
	


	// Setup signal handlers
	mast_setup_signals();


	// The main loop
	while( mast_still_running() )
	{
		int frames_read = fill_input_buffer( input, input_buffer );
		int payload_bytes = 0;
		
		// Was there an error?
		if (frames_read < 0) break;
		
		// Encode audio
		payload_bytes = codec->encode_packet( input_buffer, g_payload_size_limit, payload_buffer );
		if (payload_bytes<0)
		{
			MAST_ERROR("Codec encode failed" );
			break;
		}
		
		// We used the audio up
		input_buffer->empty_buffer();
	
		if (payload_bytes) {
			// Send out an RTP packet
			rtp_session_send_with_ts(session, payload_buffer, payload_bytes, ts);
			
			// Calculate the timestamp increment
			ts+=((frames_read * pt->clock_rate) / sfinfo.samplerate);   //  * frames_per_packet;
		}
		

		// Reached end of file?
		if (frames_read < frames_per_packet) {
			MAST_DEBUG("Reached end of file (wanted=%d, frames_read=%d)", frames_per_packet, frames_read);
			if (g_loop_file) {
				// Seek back to the beginning
				if (sf_seek( input, 0, SEEK_SET )) {
					MAST_ERROR("Failed to seek to start of file: %s", sf_strerror( input ) );
					break;
				}
			} else {
				// End of file, exit main loop
				break;
			}
		}
		
	}

	// Free up the buffers audio/read buffers
	if (payload_buffer) {
		free(payload_buffer);
		payload_buffer=NULL;
	}
	if (input_buffer) {
		delete input_buffer;
		input_buffer=NULL;
	}
	

	// Close input file
	if (sf_close( input )) {
		MAST_ERROR("Failed to close input file:\n%s", sf_strerror(input));
	}
	

	// Delete objects
	delete codec;
	delete g_mime_type;

	// Close RTP session
	mast_deinit_ortp( session );
	
	 
	// Success !
	return 0;
}

