/*
 * Code to deal with internal libopenspotify playlist retrieving
 *
 * Program flow:
 *
 * + network_thread()
 * +--+ playlist_process(REQ_TYPE_PLAYLIST_LOAD_CONTAINER)
 * |  +--+ playlist_send_playlist_container_request()
 * |  |  +--+ cmd_getplaylist()
 * |  |     +--+ channel_register() with callback playlist_container_callback()
 * |  +--- Update request->next_timeout
 * .  .
 * .  .
 * +--+ packet_read_and_process()
 * |   +--+ handle_channel()
 * |      +--+ channel_process()
 * |         +--+ playlist_container_callback()
 * |            +--- CHANNEL_DATA: Buffer XML-data
 * |            +--+ CHANNEL_END:
 * |               +--- playlist_parse_container_xml()
 * |               +--+ playlist_post_playlist_requests()
 * |               |  +--- request_post(REQ_TYPE_PLAYLIST_LOAD_PLAYLIST)
 * |               +-- request_post_set_result(REQ_TYPE_PLAYLIST_LOAD_CONTAINER)
 * .
 * .
 * +--+ playlist_process(REQ_TYPE_PLAYLIST_LOAD_PLAYLIST)
 * |  +--+ playlist_send_playlist_request()
 * |  |  +--+ cmd_getplaylist()
 * |  |     +--+ channel_register() with callback playlist_container_callback()
 * |  +--- Update request->next_timeout
 * |
 * .  .
 * .  .
 * +--+ packet_read_and_process() 
 * |   +--+ handle_channel()
 * |      +--+ channel_process()
 * |         +--+ playlist_callback()
 * |            +--- CHANNEL_DATA: Buffer XML-data
 * |            +--+ CHANNEL_END:
 * |               +--- playlist_parse_playlist_xml()
 * |               +--+ osfy_playlist_browse() ------ XXXX
 * |               |  +--- request_post(REQ_TYPE_BROWSE_PLAYLIST_TRACKS)
 * |               +--- request_post_set_result(REQ_TYPE_PLAYLIST_LOAD_PLAYLIST)
 * .  .
 * .  .
 * +--- DONE
 * |
 *     
 */

#include <string.h>
#include <zlib.h>

#include <spotify/api.h>

#include "buf.h"
#include "browse.h"
#include "channel.h"
#include "commands.h"
#include "debug.h"
#include "ezxml.h"
#include "playlist.h"
#include "request.h"
#include "sp_opaque.h"
#include "track.h"
#include "util.h"


static int playlist_send_playlist_container_request(sp_session *session, struct request *req);
static int playlist_container_callback(CHANNEL *ch, unsigned char *payload, unsigned short len);
static int playlist_parse_container_xml(sp_session *session);

static void playlist_post_playlist_requests(sp_session *session);
static int playlist_send_playlist_request(sp_session *session, struct request *req);
static int playlist_callback(CHANNEL *ch, unsigned char *payload, unsigned short len);
static int playlist_parse_playlist_xml(sp_session *session, sp_playlist *playlist);

static int osfy_playlist_browse(sp_session *session, sp_playlist *playlist);
static int osfy_playlist_browse_callback(struct browse_callback_ctx *brctx);
#if 0
static void playlist_post_track_request(sp_session *session, sp_playlist *);
#endif

unsigned long playlist_checksum(sp_playlist *playlist);
unsigned long playlistcontainer_checksum(sp_playlistcontainer *container);


/* For giving the channel handler access to both the session and the request */
struct callback_ctx {
	sp_session *session;
	struct request *req;
};


/* Initialize a playlist context, called by sp_session_init() */
struct playlist_ctx *playlist_create(void) {
	struct playlist_ctx *playlist_ctx;

	playlist_ctx = malloc(sizeof(struct playlist_ctx));
	if(playlist_ctx == NULL)
		return NULL;

	playlist_ctx->buf = NULL;

	playlist_ctx->container = malloc(sizeof(sp_playlistcontainer));
	playlist_ctx->container->playlists = NULL;

	/* FIXME: Should be an array of callbacks and userdatas */
	playlist_ctx->container->userdata = NULL;
	playlist_ctx->container->callbacks = malloc(sizeof(sp_playlistcontainer_callbacks));
	memset(playlist_ctx->container->callbacks, 0, sizeof(sp_playlistcontainer_callbacks));

	return playlist_ctx;
}


/* Release resources held by the playlist context, called by sp_session_release() */
void playlist_release(struct playlist_ctx *playlist_ctx) {
	sp_playlist *playlist, *next_playlist;
	int i;

	if(playlist_ctx->container) {
		for(playlist = playlist_ctx->container->playlists;
			playlist; playlist = next_playlist) {

			if(playlist->buf)
				buf_free(playlist->buf);

			if(playlist->name)
				free(playlist->name);

			if(playlist->owner) {
				/* FIXME: Free sp_user */
			}

			for(i = 0; i < playlist->num_tracks; i++)
				sp_track_release(playlist->tracks[i]);

			if(playlist->num_tracks)
				free(playlist->tracks);

			if(playlist->callbacks)
				free(playlist->callbacks);

			next_playlist = playlist->next;
			free(playlist);
		}

		free(playlist_ctx->container->callbacks);
		free(playlist_ctx->container);
	}

	if(playlist_ctx->buf)
		buf_free(playlist_ctx->buf);

	free(playlist_ctx);
}


/* Playlist FSM */
int playlist_process(sp_session *session, struct request *req) {
	int ret;

	if(req->state == REQ_STATE_NEW)
		req->state = REQ_STATE_RUNNING;

	if(req->type == REQ_TYPE_PLAYLIST_LOAD_CONTAINER) {
		req->next_timeout = get_millisecs() + PLAYLIST_RETRY_TIMEOUT*1000;

		/* Send request (CMD_GETPLAYLIST) to load playlist container */
		ret = playlist_send_playlist_container_request(session, req);
	}
	else if(req->type == REQ_TYPE_PLAYLIST_LOAD_PLAYLIST) {
		req->next_timeout = get_millisecs() + PLAYLIST_RETRY_TIMEOUT*1000;

		/* Send request (CMD_GETPLAYLIST) to load playlist */
		ret = playlist_send_playlist_request(session, req);
	}

	return ret;
}


/* Request playlist container */
static int playlist_send_playlist_container_request(sp_session *session, struct request *req) {
	unsigned char container_id[17];
	static const char* decl_and_root =
		"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<playlist>\n";

	struct callback_ctx *callback_ctx;


	DSFYDEBUG("Requesting playlist container\n");

	/* Free'd by the callback */
	callback_ctx = malloc(sizeof(struct callback_ctx));
	callback_ctx->session = session;
	callback_ctx->req = req;

	session->playlist_ctx->buf = buf_new();
	buf_append_data(session->playlist_ctx->buf, (char*)decl_and_root, strlen(decl_and_root));

	memset(container_id, 0, 17);
	return cmd_getplaylist(session, container_id, ~0, 
				playlist_container_callback, callback_ctx);
}


/* Callback for playlist container buffering */
static int playlist_container_callback(CHANNEL *ch, unsigned char *payload, unsigned short len) {
	struct callback_ctx *callback_ctx = (struct callback_ctx *)ch->private;
	struct playlist_ctx *playlist_ctx = callback_ctx->session->playlist_ctx;

	switch(ch->state) {
	case CHANNEL_DATA:
		buf_append_data(playlist_ctx->buf, payload, len);
		break;

	case CHANNEL_ERROR:
		buf_free(playlist_ctx->buf);
		playlist_ctx->buf = NULL;

		/* Don't set error on request. It will be retried. */
		free(callback_ctx);

		DSFYDEBUG("Got a channel ERROR\n");
		break;

	case CHANNEL_END:
		/* Parse returned XML and request each listed playlist */
		if(playlist_parse_container_xml(callback_ctx->session) == 0) {

			/* Create new requests for each playlist found */
			playlist_post_playlist_requests(callback_ctx->session);

			/* Note we're done loading the playlist container */
			request_set_result(callback_ctx->session, callback_ctx->req, SP_ERROR_OK, NULL);
		}

		free(callback_ctx);

		buf_free(playlist_ctx->buf);
		playlist_ctx->buf = NULL;

		break;

	default:
		break;
	}

	return 0;
}


static int playlist_parse_container_xml(sp_session *session) {
	static char *end_element = "</playlist>";
	char *id_list, *id;
	char idstr[35];
	int position;
	ezxml_t root, node;
	sp_playlist *playlist;
	sp_playlistcontainer *container;

	buf_append_data(session->playlist_ctx->buf, end_element, strlen(end_element));
	buf_append_u8(session->playlist_ctx->buf, 0);

	root = ezxml_parse_str((char *)session->playlist_ctx->buf->ptr, session->playlist_ctx->buf->len);
	node = ezxml_get(root, "next-change", 0, "change", 0, "ops", 0, "add", 0, "items", -1);
	id_list = node->txt;

	container = session->playlist_ctx->container;
	for(id = strtok(id_list, ",\n"); id; id = strtok(NULL, ",\n")) {
		hex_bytes_to_ascii((unsigned char *)id, idstr, 17);
		DSFYDEBUG("Playlist ID '%s'\n", idstr);
	
		position = 0;
		if((playlist = container->playlists) == NULL) {
			container->playlists = malloc(sizeof(sp_playlist));
			playlist = container->playlists;
		}
		else {
			for(position = 0; playlist->next; position++)
				playlist = playlist->next;

			playlist->next = malloc(sizeof(sp_playlist));
			playlist = playlist->next;
		}

		hex_ascii_to_bytes(id, playlist->id, sizeof(playlist->id));
		playlist->num_tracks = 0;
		playlist->tracks = NULL;

		/* FIXME: Pull this info from XML ? */
		playlist->name = NULL;
		playlist->owner = NULL;

		playlist->lastrequest = 0;
		playlist->state = PLAYLIST_STATE_ADDED;

		playlist->callbacks = NULL;
		playlist->userdata = NULL;

		playlist->buf = NULL;
		playlist->next = NULL;

		/* FIXME: Probably shouldn't carry around playlist positions in the playlist itself! */
		playlist->position = position;
	}
	

	ezxml_free(root);

	return 0;
}


/* Create new requests for each playlist in the container */
static void playlist_post_playlist_requests(sp_session *session) {
	sp_playlist *playlist, **ptr;
	int i;

	i = 0;
	for(playlist = session->playlist_ctx->container->playlists;
		playlist; playlist = playlist->next) {

		ptr = (sp_playlist **)malloc(sizeof(sp_playlist *));
		*ptr = playlist;
		request_post(session, REQ_TYPE_PLAYLIST_LOAD_PLAYLIST, ptr);

		i++;
	}

	DSFYDEBUG("Created %d requests to retrieve playlists\n", i);
}


/* Request a playlist from Spotify */
static int playlist_send_playlist_request(sp_session *session, struct request *req) {
	int ret;
	char idstr[35];
	struct callback_ctx *callback_ctx;
	sp_playlist *playlist = *(sp_playlist **)req->input;
	static const char* decl_and_root =
		"<?xml version=\"1.0\" encoding=\"utf-8\" ?>\n<playlist>\n";

	callback_ctx = malloc(sizeof(struct callback_ctx));
	callback_ctx->session = session;
	callback_ctx->req = req;

	playlist->buf = buf_new();
	buf_append_data(playlist->buf, (char*)decl_and_root, strlen(decl_and_root));
	
	hex_bytes_to_ascii((unsigned char *)playlist->id, idstr, 17);

	ret =  cmd_getplaylist(session, playlist->id, ~0, 
			playlist_callback, callback_ctx);

	DSFYDEBUG("Sent request for playlist with ID '%s' at time %d\n", idstr, get_millisecs());
	return ret;
}



/* Callback for playlist container buffering */
static int playlist_callback(CHANNEL *ch, unsigned char *payload, unsigned short len) {
	struct callback_ctx *callback_ctx = (struct callback_ctx *)ch->private;
	sp_playlist *playlist = *(sp_playlist **)callback_ctx->req->input;

	switch(ch->state) {
	case CHANNEL_DATA:
		buf_append_data(playlist->buf, payload, len);
		break;

	case CHANNEL_ERROR:
		buf_free(playlist->buf);
		playlist->buf = NULL;

		/* Don't set error on request. It will be retried. */
		free(callback_ctx);

		DSFYDEBUG("Got a channel ERROR\n");
		break;

	case CHANNEL_END:
		/* Parse returned XML and request tracks */
		if(playlist_parse_playlist_xml(callback_ctx->session, playlist) == 0) {
			playlist->state = PLAYLIST_STATE_LISTED;

			/* Create new request for loading tracks */
#if 0
			playlist_post_track_request(callback_ctx->session, playlist);
#endif
			osfy_playlist_browse(callback_ctx->session, playlist);
			
			/* Note we're done loading this playlist */
			request_set_result(callback_ctx->session, callback_ctx->req, SP_ERROR_OK, playlist);

			{
				char idstr[35];
				hex_bytes_to_ascii((unsigned char *)playlist->id, idstr, 17);
				DSFYDEBUG("Successfully loaded playlist '%s'\n", idstr);
			}
		}

		buf_free(playlist->buf);
		playlist->buf = NULL;

		free(callback_ctx);
		break;

	default:
		break;
	}

	return 0;
}


static int playlist_parse_playlist_xml(sp_session *session, sp_playlist *playlist) {
	static char *end_element = "</playlist>";
	char *id_list, *idstr;
	unsigned char track_id[16];
	ezxml_t root, node;
	sp_track *track;

	/* NULL-terminate XML */
	buf_append_data(playlist->buf, end_element, strlen(end_element));
	buf_append_u8(playlist->buf, 0);

	root = ezxml_parse_str((char *)playlist->buf->ptr, playlist->buf->len);
	node = ezxml_get(root, "next-change", 0, "change", 0, "ops", 0, "add", 0, "items", -1);
	id_list = node->txt;
	
	for(idstr = strtok(id_list, ",\n"); idstr; idstr = strtok(NULL, ",\n")) {
		hex_ascii_to_bytes(idstr, track_id, sizeof(track_id));
		track = osfy_track_add(session, track_id);

		playlist->tracks = (sp_track **)realloc(playlist->tracks, (playlist->num_tracks + 1) * sizeof(sp_track *));
		playlist->tracks[playlist->num_tracks] = track;
		playlist->num_tracks++;

		sp_track_add_ref(track);
	}
	
	ezxml_free(root);

	return 0;
}


/* Create new track request for this playlist */
void playlist_post_track_request(sp_session *session, sp_playlist *playlist) {
	sp_playlist **ptr;

	ptr = (sp_playlist **)malloc(sizeof(sp_playlist *));
	*ptr = playlist;

	request_post(session, REQ_TYPE_BROWSE_TRACK, ptr);
}



/*
 * Initiate track browsing of a single playlist
 *
 */
int osfy_playlist_browse(sp_session *session, sp_playlist *playlist) {
	int i;
	void **container;
	struct browse_callback_ctx *brctx;
	
	/*
	 * Temporarily increase ref count for the artist so it's not free'd
	 * accidentily. It will be decreaed by the chanel callback.
	 *
	 */
	for(i = 0; i < playlist->num_tracks; i++)
		sp_track_add_ref(playlist->tracks[i]);

	
	/* The playlist callback context */
	brctx = (struct browse_callback_ctx *)malloc(sizeof(struct browse_callback_ctx));
	
	brctx->session = session;
	brctx->req = NULL; /* Filled in by the request processor */
	brctx->buf = NULL; /* Filled in by the request processor */
	
	brctx->type = REQ_TYPE_BROWSE_PLAYLIST_TRACKS;
	brctx->data.playlist = playlist;
	brctx->num_total = playlist->num_tracks;
	brctx->num_browsed = 0;
	brctx->num_in_request = 0;
	
	
	/* Our gzip'd XML parser */
	brctx->browse_parser = osfy_playlist_browse_callback;
	
	/* Request input container. Will be free'd when the request is finished. */
	container = (void **)malloc(sizeof(void *));
	*container = brctx;
	
	return request_post(session, REQ_TYPE_BROWSE_ALBUM, container);
}


static int osfy_playlist_browse_callback(struct browse_callback_ctx *brctx) {
	int i;
	struct buf *xml;
	unsigned char id[16];
	ezxml_t root, track_node, node;
	sp_track *track;
	
	
	/* Decompress the XML returned by track browsing */
	xml = despotify_inflate(brctx->buf->ptr, brctx->buf->len);
	{
		FILE *fd;
		char buf[35];
		hex_bytes_to_ascii(brctx->data.playlist->id, buf, 17);
		DSFYDEBUG("Decompresed %d bytes data for playlist '%s', xml=%p, saving raw XML to browse-playlist.xml\n",
			  brctx->buf->len, buf, xml);
		fd = fopen("browse-playlists.xml", "w");
		if(fd) {
			fwrite(xml->ptr, xml->len, 1, fd);
			fclose(fd);
		}
	}
	

	/* Load XML */
	root = ezxml_parse_str((char *) xml->ptr, xml->len);
	if(root == NULL) {
		DSFYDEBUG("Failed to parse XML\n");
		buf_free(xml);
		return -1;
	}
	

	/* Loop over each track in the list */
	for(i = 1, track_node = ezxml_get(root, "tracks", 0, "track", -1);
	    track_node;
	    track_node = track_node->next, i++) {
		
		/* Get ID of track */
		node = ezxml_get(track_node, "id", -1);
		hex_ascii_to_bytes(node->txt, id, 16);
		
		/* We'll simply use ofsy_track_add() to find a track by its ID */
		track = osfy_track_add(brctx->session, id);
		{
			char buf[33];
			hex_bytes_to_ascii(track->id, buf, 16);
			DSFYDEBUG("osfy_track_add(%s) gave track with ID '%s'\n", node->txt, buf);
		}
		
		
		
		/* Skip loading of already loaded tracks */
		if(sp_track_is_loaded(track)) {
			DSFYDEBUG("Track '%s' (%d in playlist) is already loaded\n", node->txt, i);
			continue;
		}
		
		{
		char buf[35];
		hex_bytes_to_ascii(brctx->data.playlist->id, buf, 17);
		DSFYDEBUG("Loading track '%s' (ref_count %d) in playlist '%s' (track number %d)\n", node->txt, track->ref_count, buf, i);
		
		}

		/* Load the track from XML */
		osfy_track_load_from_xml(brctx->session, track, track_node);
	}

	/* Free XML structures and buffer */
	ezxml_free(root);
	buf_free(xml);

	
	/* Release references made in osfy_playlist_browse() */
	for(i = 0; i < brctx->num_in_request; i++)
		sp_track_release(brctx->data.playlist->tracks[brctx->num_browsed + i]);
	
	
	return 0;
}


/* Calculate a playlist checksum. */
unsigned long playlist_checksum(sp_playlist *playlist) {
	unsigned long checksum = 1L;
	int i;

	if(playlist == NULL)
		return 1L;

	/* Loop over all tracks (make sure the last byte is 0x01). */
	for(i = 0; i < playlist->num_tracks; i++){
		playlist->tracks[i]->id[16] = 0x01;

		checksum = adler32(checksum, playlist->tracks[i]->id, 17);
	}

	return checksum;
}


/* Calculate a playlists container checksum. */
unsigned long playlistcontainer_checksum(sp_playlistcontainer *container) {
	unsigned long checksum = 1L;
	sp_playlist *playlist;

	if(container == NULL)
		return 1L;

	/* Loop over all playlists (make sure the last byte is 0x02). */
	for(playlist = container->playlists; playlist != NULL; playlist = playlist->next){
		playlist->id[16] = 0x02;

		checksum = adler32(checksum, playlist->id, 17);
	}

	return checksum;
}
