/*! \file    record.h
 * \author   Lorenzo Miniero <lorenzo@meetecho.com>
 * \copyright GNU General Public License v3
 * \brief    Audio/Video recorder
 * \details  Implementation of a simple recorder utility that plugins
 * can make use of to record audio/video frames to a Janus file. This
 * file just saves RTP frames in a structured way, so that they can be
 * post-processed later on to get a valid container file (e.g., a .opus
 * file for Opus audio or a .webm file for VP8 video) and keep things
 * simpler on the plugin and core side. Check the \ref recordings
 * documentation for more details.
 * \note If you want to record both audio and video, you'll have to use
 * two different recorders. Any muxing in the same container will have
 * to be done in the post-processing phase.
 *
 * \ingroup core
 * \ref core
 */

#include <arpa/inet.h>
#include <sys/stat.h>
#include <errno.h>
#include <libgen.h>

#include <glib.h>
#include <jansson.h>

#include "record.h"
#include "debug.h"
#include "utils.h"


/* Info header in the structured recording */
static const char *header = "MJR00002";
/* Frame header in the structured recording */
static const char *frame_header = "MEET";

/* Whether the filenames should have a temporary extension, while saving, or not (default=false) */
static gboolean rec_tempname = FALSE;
/* Extension to add in case tempnames is true (default="tmp" --> ".tmp") */
static char *rec_tempext = NULL;

void janus_recorder_init(gboolean tempnames, const char *extension) {
	JANUS_LOG(LOG_INFO, "Initializing recorder code\n");
	if(tempnames) {
		rec_tempname = TRUE;
		if(extension == NULL) {
			rec_tempext = g_strdup("tmp");
			JANUS_LOG(LOG_INFO, "  -- No extension provided, using default one (tmp)\n");
		} else {
			rec_tempext = g_strdup(extension);
			JANUS_LOG(LOG_INFO, "  -- Using temporary extension .%s\n", rec_tempext);
		}
	}
}

void janus_recorder_deinit(void) {
	rec_tempname = FALSE;
	g_free(rec_tempext);
}

static void janus_recorder_free(const janus_refcount *recorder_ref) {
	janus_recorder *recorder = janus_refcount_containerof(recorder_ref, janus_recorder, ref);
	/* This recorder can be destroyed, free all the resources */
	janus_recorder_close(recorder);
	g_free(recorder->dir);
	recorder->dir = NULL;
	g_free(recorder->filename);
	recorder->filename = NULL;
	if(recorder->file != NULL)
		fclose(recorder->file);
	recorder->file = NULL;
	g_free(recorder->codec);
	recorder->codec = NULL;
	g_free(recorder->fmtp);
	recorder->fmtp = NULL;
	g_free(recorder->description);
	recorder->description = NULL;
	if(recorder->extensions != NULL)
		g_hash_table_destroy(recorder->extensions);
	janus_mutex_destroy(&recorder->mutex);
	g_free(recorder);
}

janus_recorder *janus_recorder_create(const char *dir, const char *codec, const char *filename) {
	/* Same as janus_recorder_create_full, but with no fmtp */
	return janus_recorder_create_full(dir, codec, NULL, filename);
}
janus_recorder *janus_recorder_create_full(const char *dir, const char *codec, const char *fmtp, const char *filename) {
	janus_recorder_medium type = JANUS_RECORDER_AUDIO;
	if(codec == NULL) {
		JANUS_LOG(LOG_ERR, "Missing codec information\n");
		return NULL;
	}
	if(!strcasecmp(codec, "vp8") || !strcasecmp(codec, "vp9") || !strcasecmp(codec, "h264")
			 || !strcasecmp(codec, "av1") || !strcasecmp(codec, "h265")) {
		type = JANUS_RECORDER_VIDEO;
	} else if(!strcasecmp(codec, "opus") || !strcasecmp(codec, "multiopus")
			|| !strcasecmp(codec, "g711") || !strcasecmp(codec, "pcmu") || !strcasecmp(codec, "pcma")
			|| !strcasecmp(codec, "g722") || !strcasecmp(codec, "l16-48") || !strcasecmp(codec, "l16")) {
		type = JANUS_RECORDER_AUDIO;
	} else if(!strcasecmp(codec, "text") || !strcasecmp(codec, "binary")) {
		/* Data channels may be text or binary, so that's what we can save too */
		type = JANUS_RECORDER_DATA;
	} else {
		/* We don't recognize the codec: while we might go on anyway, we'd rather fail instead */
		JANUS_LOG(LOG_ERR, "Unsupported codec '%s'\n", codec);
		return NULL;
	}
	/* Create the recorder */
	janus_recorder *rc = g_malloc0(sizeof(janus_recorder));
	janus_refcount_init(&rc->ref, janus_recorder_free);
	janus_rtp_switching_context_reset(&rc->context);
	rc->dir = NULL;
	rc->filename = NULL;
	rc->file = NULL;
	rc->codec = g_strdup(codec);
	rc->fmtp = fmtp ? g_strdup(fmtp) : NULL;
	rc->description = NULL;
	rc->created = janus_get_real_time();
	janus_mutex_init(&rc->mutex);
	const char *rec_dir = NULL;
	const char *rec_file = NULL;
	char *copy_for_parent = NULL;
	char *copy_for_base = NULL;
	/* Check dir and filename values */
	if(filename != NULL) {
		/* Helper copies to avoid overwriting */
		copy_for_parent = g_strdup(filename);
		copy_for_base = g_strdup(filename);
		/* Get filename parent folder */
		const char *filename_parent = dirname(copy_for_parent);
		/* Get filename base file */
		const char *filename_base = basename(copy_for_base);
		if(!dir) {
			/* If dir is NULL we have to create filename_parent and filename_base */
			rec_dir = filename_parent;
			rec_file = filename_base;
		} else {
			/* If dir is valid we have to create dir and filename*/
			rec_dir = dir;
			rec_file = filename;
			if(strcasecmp(filename_parent, ".") || strcasecmp(filename_base, filename)) {
				JANUS_LOG(LOG_WARN, "Unsupported combination of dir and filename %s %s\n", dir, filename);
			}
		}
	}
	if(rec_dir != NULL) {
		/* Check if this directory exists, and create it if needed */
		struct stat s;
		int err = stat(rec_dir, &s);
		if(err == -1) {
			if(ENOENT == errno) {
				/* Directory does not exist, try creating it */
				if(janus_mkdir(rec_dir, 0755) < 0) {
					JANUS_LOG(LOG_ERR, "mkdir (%s) error: %d (%s)\n", rec_dir, errno, g_strerror(errno));
					janus_recorder_destroy(rc);
					g_free(copy_for_parent);
					g_free(copy_for_base);
					return NULL;
				}
			} else {
				JANUS_LOG(LOG_ERR, "stat (%s) error: %d (%s)\n", rec_dir, errno, g_strerror(errno));
				janus_recorder_destroy(rc);
				g_free(copy_for_parent);
				g_free(copy_for_base);
				return NULL;
			}
		} else {
			if(S_ISDIR(s.st_mode)) {
				/* Directory exists */
				JANUS_LOG(LOG_VERB, "Directory exists: %s\n", rec_dir);
			} else {
				/* File exists but it's not a directory? */
				JANUS_LOG(LOG_ERR, "Not a directory? %s\n", rec_dir);
				janus_recorder_destroy(rc);
				g_free(copy_for_parent);
				g_free(copy_for_base);
				return NULL;
			}
		}
	}
	char newname[1024];
	memset(newname, 0, 1024);
	if(rec_file == NULL) {
		/* Choose a random username */
		if(!rec_tempname) {
			/* Use .mjr as an extension right away */
			g_snprintf(newname, 1024, "janus-recording-%"SCNu32".mjr", janus_random_uint32());
		} else {
			/* Append the temporary extension to .mjr, we'll rename when closing */
			g_snprintf(newname, 1024, "janus-recording-%"SCNu32".mjr.%s", janus_random_uint32(), rec_tempext);
		}
	} else {
		/* Just append the extension */
		if(!rec_tempname) {
			/* Use .mjr as an extension right away */
			g_snprintf(newname, 1024, "%s.mjr", rec_file);
		} else {
			/* Append the temporary extension to .mjr, we'll rename when closing */
			g_snprintf(newname, 1024, "%s.mjr.%s", rec_file, rec_tempext);
		}
	}
	/* Try opening the file now */
	if(rec_dir == NULL) {
		/* Make sure folder to save to is not protected */
		if(janus_is_folder_protected(newname)) {
			JANUS_LOG(LOG_ERR, "Target recording path '%s' is in protected folder...\n", newname);
			janus_recorder_destroy(rc);
			g_free(copy_for_parent);
			g_free(copy_for_base);
			return NULL;
		}
		rc->file = fopen(newname, "wb");
	} else {
		char path[1024];
		memset(path, 0, 1024);
		g_snprintf(path, 1024, "%s/%s", rec_dir, newname);
		/* Make sure folder to save to is not protected */
		if(janus_is_folder_protected(path)) {
			JANUS_LOG(LOG_ERR, "Target recording path '%s' is in protected folder...\n", path);
			janus_recorder_destroy(rc);
			g_free(copy_for_parent);
			g_free(copy_for_base);
			return NULL;
		}
		rc->file = fopen(path, "wb");
	}
	if(rc->file == NULL) {
		JANUS_LOG(LOG_ERR, "fopen error: %d\n", errno);
		janus_recorder_destroy(rc);
		g_free(copy_for_parent);
		g_free(copy_for_base);
		return NULL;
	}
	if(rec_dir)
		rc->dir = g_strdup(rec_dir);
	rc->filename = g_strdup(newname);
	rc->type = type;
	/* Write the first part of the header */
	size_t res = fwrite(header, sizeof(char), strlen(header), rc->file);
	if(res != strlen(header)) {
		JANUS_LOG(LOG_ERR, "Couldn't write .mjr header (%zu != %zu, %s)\n",
			res, strlen(header), g_strerror(errno));
		janus_recorder_destroy(rc);
		g_free(copy_for_parent);
		g_free(copy_for_base);
		return NULL;
	}
	g_atomic_int_set(&rc->writable, 1);
	/* We still need to also write the info header first */
	g_atomic_int_set(&rc->header, 0);
	/* Done */
	g_atomic_int_set(&rc->destroyed, 0);
	g_free(copy_for_parent);
	g_free(copy_for_base);
	return rc;
}

int janus_recorder_pause(janus_recorder *recorder) {
	if(!recorder)
		return -1;
	if(g_atomic_int_compare_and_exchange(&recorder->paused, 0, 1))
		return 0;
	return -2;
}

int janus_recorder_resume(janus_recorder *recorder) {
	if(!recorder)
		return -1;
	janus_mutex_lock_nodebug(&recorder->mutex);
	if(g_atomic_int_compare_and_exchange(&recorder->paused, 1, 0)) {
		if(recorder->type == JANUS_RECORDER_AUDIO || recorder->type == JANUS_RECORDER_VIDEO) {
			recorder->context.ts_reset = TRUE;
			recorder->context.seq_reset = TRUE;
			recorder->context.last_time = janus_get_monotonic_time();
		}
		janus_mutex_unlock_nodebug(&recorder->mutex);
		return 0;
	}
	janus_mutex_unlock_nodebug(&recorder->mutex);
	return -2;
}

int janus_recorder_add_extmap(janus_recorder *recorder, int id, const char *extmap) {
	if(!recorder || g_atomic_int_get(&recorder->header) || id < 1 || id > 15 || extmap == NULL)
		return -1;
	janus_mutex_lock_nodebug(&recorder->mutex);
	if(recorder->extensions == NULL)
		recorder->extensions = g_hash_table_new_full(NULL, NULL, NULL, (GDestroyNotify)g_free);
	g_hash_table_insert(recorder->extensions, GINT_TO_POINTER(id), g_strdup(extmap));
	janus_mutex_unlock_nodebug(&recorder->mutex);
	return 0;
}

int janus_recorder_description(janus_recorder *recorder, const char *description) {
	if(!recorder || !description)
		return -1;
	janus_mutex_lock_nodebug(&recorder->mutex);
	if(g_atomic_int_get(&recorder->header)) {
		/* No use setting description once it's already written in the MJR file */
		janus_mutex_unlock_nodebug(&recorder->mutex);
		return 0;
	}
	g_free(recorder->description);
	recorder->description = g_strdup(description);
	janus_mutex_unlock_nodebug(&recorder->mutex);
	return 0;
}

int janus_recorder_opusred(janus_recorder *recorder, int red_pt) {
	if(!recorder)
		return -1;
	if(!g_atomic_int_get(&recorder->header)) {
		recorder->opusred_pt = red_pt;
		return 0;
	}
	return -1;
}

int janus_recorder_encrypted(janus_recorder *recorder) {
	if(!recorder)
		return -1;
	if(!g_atomic_int_get(&recorder->header)) {
		recorder->encrypted = TRUE;
		return 0;
	}
	return -1;
}

int janus_recorder_save_frame(janus_recorder *recorder, char *buffer, uint length) {
	if(!recorder)
		return -1;
	janus_mutex_lock_nodebug(&recorder->mutex);
	if(!buffer || length < 1) {
		janus_mutex_unlock_nodebug(&recorder->mutex);
		return -2;
	}
	if(!recorder->file) {
		janus_mutex_unlock_nodebug(&recorder->mutex);
		return -3;
	}
	if(!g_atomic_int_get(&recorder->writable)) {
		janus_mutex_unlock_nodebug(&recorder->mutex);
		return -4;
	}
	if(g_atomic_int_get(&recorder->paused)) {
		janus_mutex_unlock_nodebug(&recorder->mutex);
		return -5;
	}
	gint64 now = janus_get_monotonic_time();
	if(!g_atomic_int_get(&recorder->header)) {
		/* Write info header as a JSON formatted info */
		json_t *info = json_object();
		/* FIXME Codecs should be configurable in the future */
		const char *type = NULL;
		if(recorder->type == JANUS_RECORDER_AUDIO)
			type = "a";
		else if(recorder->type == JANUS_RECORDER_VIDEO)
			type = "v";
		else if(recorder->type == JANUS_RECORDER_DATA)
			type = "d";
		json_object_set_new(info, "t", json_string(type));								/* Audio/Video/Data */
		json_object_set_new(info, "c", json_string(recorder->codec));					/* Media codec */
		if(recorder->fmtp)
			json_object_set_new(info, "f", json_string(recorder->fmtp));				/* Codec-specific info */
		if(recorder->description)
			json_object_set_new(info, "d", json_string(recorder->description));		/* Stream description */
		if(recorder->extensions) {
			/* Add the extmaps to the JSON object */
			json_t *extmaps = NULL;
			GHashTableIter iter;
			gpointer key, value;
			g_hash_table_iter_init(&iter, recorder->extensions);
			while(g_hash_table_iter_next(&iter, &key, &value)) {
				int id = GPOINTER_TO_INT(key);
				char *extmap = (char *)value;
				if(id > 0 && id < 16 && extmap != NULL) {
					if(extmaps == NULL)
						extmaps = json_object();
					char id_str[3];
					g_snprintf(id_str, sizeof(id_str), "%d", id);
					json_object_set_new(extmaps, id_str, json_string(extmap));
				}
			}
			if(extmaps != NULL)
				json_object_set_new(info, "x", extmaps);
		}
		json_object_set_new(info, "s", json_integer(recorder->created));				/* Created time */
		json_object_set_new(info, "u", json_integer(janus_get_real_time()));			/* First frame written time */
		/* If this is audio and using RED, take note of the payload type */
		if(recorder->type == JANUS_RECORDER_AUDIO && recorder->opusred_pt > 0)
			json_object_set_new(info, "or", json_integer(recorder->opusred_pt));
		/* If media will be end-to-end encrypted, mark it in the recording header */
		if(recorder->encrypted)
			json_object_set_new(info, "e", json_true());
		gchar *info_text = json_dumps(info, JSON_PRESERVE_ORDER);
		json_decref(info);
		if(info_text == NULL) {
			JANUS_LOG(LOG_ERR, "Error converting header to text...\n");
			janus_mutex_unlock_nodebug(&recorder->mutex);
			return -5;
		}
		uint16_t info_bytes = htons(strlen(info_text));
		size_t res = fwrite(&info_bytes, sizeof(uint16_t), 1, recorder->file);
		if(res != 1) {
			JANUS_LOG(LOG_WARN, "Couldn't write size of JSON header in .mjr file (%zu != %zu, %s), expect issues post-processing\n",
				res, sizeof(uint16_t), g_strerror(errno));
		}
		res = fwrite(info_text, sizeof(char), strlen(info_text), recorder->file);
		if(res != strlen(info_text)) {
			JANUS_LOG(LOG_WARN, "Couldn't write JSON header in .mjr file (%zu != %zu, %s), expect issues post-processing\n",
				res, strlen(info_text), g_strerror(errno));
		}
		free(info_text);
		/* Done */
		recorder->started = now;
		g_atomic_int_set(&recorder->header, 1);
	}
	/* Write frame header (fixed part[4], timestamp[4], length[2]) */
	size_t res = fwrite(frame_header, sizeof(char), strlen(frame_header), recorder->file);
	if(res != strlen(frame_header)) {
		JANUS_LOG(LOG_WARN, "Couldn't write frame header in .mjr file (%zu != %zu, %s), expect issues post-processing\n",
			res, strlen(frame_header), g_strerror(errno));
	}
	uint32_t timestamp = (uint32_t)(now > recorder->started ? ((now - recorder->started)/1000) : 0);
	timestamp = htonl(timestamp);
	res = fwrite(&timestamp, sizeof(uint32_t), 1, recorder->file);
	if(res != 1) {
		JANUS_LOG(LOG_WARN, "Couldn't write frame timestamp in .mjr file (%zu != %zu, %s), expect issues post-processing\n",
			res, sizeof(uint32_t), g_strerror(errno));
	}
	uint16_t header_bytes = htons(recorder->type == JANUS_RECORDER_DATA ? (length+sizeof(gint64)) : length);
	res = fwrite(&header_bytes, sizeof(uint16_t), 1, recorder->file);
	if(res != 1) {
		JANUS_LOG(LOG_WARN, "Couldn't write size of frame in .mjr file (%zu != %zu, %s), expect issues post-processing\n",
			res, sizeof(uint16_t), g_strerror(errno));
	}
	if(recorder->type == JANUS_RECORDER_DATA) {
		/* If it's data, then we need to prepend timing related info, as it's not there by itself */
		gint64 now = htonll((uint64_t)janus_get_real_time());
		res = fwrite(&now, sizeof(gint64), 1, recorder->file);
		if(res != 1) {
			JANUS_LOG(LOG_WARN, "Couldn't write data timestamp in .mjr file (%zu != %zu, %s), expect issues post-processing\n",
				res, sizeof(gint64), g_strerror(errno));
		}
	}
	/* Edit packet header if needed */
	janus_rtp_header *header = (janus_rtp_header *)buffer;
	uint32_t ssrc = 0;
	uint16_t seq = 0;
	if(recorder->type != JANUS_RECORDER_DATA) {
		ssrc = ntohl(header->ssrc);
		seq = ntohs(header->seq_number);
		timestamp = ntohl(header->timestamp);
		janus_rtp_header_update(header, &recorder->context, recorder->type == JANUS_RECORDER_VIDEO, 0);
	}
	/* Save packet on file */
	int temp = 0, tot = length;
	while(tot > 0) {
		temp = fwrite(buffer+length-tot, sizeof(char), tot, recorder->file);
		if(temp <= 0) {
			JANUS_LOG(LOG_ERR, "Error saving frame...\n");
			if(recorder->type != JANUS_RECORDER_DATA) {
				/* Restore packet header data */
				header->ssrc = htonl(ssrc);
				header->seq_number = htons(seq);
				header->timestamp = htonl(timestamp);
			}
			janus_mutex_unlock_nodebug(&recorder->mutex);
			return -6;
		}
		tot -= temp;
	}
	if(recorder->type != JANUS_RECORDER_DATA) {
		/* Restore packet header data */
		header->ssrc = htonl(ssrc);
		header->seq_number = htons(seq);
		header->timestamp = htonl(timestamp);
	}
	/* Done */
	janus_mutex_unlock_nodebug(&recorder->mutex);
	return 0;
}

int janus_recorder_close(janus_recorder *recorder) {
	if(!recorder || !g_atomic_int_compare_and_exchange(&recorder->writable, 1, 0))
		return -1;
	janus_mutex_lock_nodebug(&recorder->mutex);
	if(recorder->file) {
		fseek(recorder->file, 0L, SEEK_END);
		size_t fsize = ftell(recorder->file);
		fseek(recorder->file, 0L, SEEK_SET);
		JANUS_LOG(LOG_INFO, "File is %zu bytes: %s\n", fsize, recorder->filename);
	}
	if(rec_tempname) {
		/* We need to rename the file, to remove the temporary extension */
		char newname[1024];
		memset(newname, 0, 1024);
		g_snprintf(newname, strlen(recorder->filename)-strlen(rec_tempext), "%s", recorder->filename);
		char oldpath[1024];
		memset(oldpath, 0, 1024);
		char newpath[1024];
		memset(newpath, 0, 1024);
		if(recorder->dir) {
			g_snprintf(newpath, 1024, "%s/%s", recorder->dir, newname);
			g_snprintf(oldpath, 1024, "%s/%s", recorder->dir, recorder->filename);
		} else {
			g_snprintf(newpath, 1024, "%s", newname);
			g_snprintf(oldpath, 1024, "%s", recorder->filename);
		}
		if(rename(oldpath, newpath) != 0) {
			JANUS_LOG(LOG_ERR, "Error renaming %s to %s...\n", recorder->filename, newname);
		} else {
			JANUS_LOG(LOG_INFO, "Recording renamed: %s\n", newname);
			g_free(recorder->filename);
			recorder->filename = g_strdup(newname);
		}
	}
	janus_mutex_unlock_nodebug(&recorder->mutex);
	return 0;
}

void janus_recorder_destroy(janus_recorder *recorder) {
	if(!recorder || !g_atomic_int_compare_and_exchange(&recorder->destroyed, 0, 1))
		return;
	janus_refcount_decrease(&recorder->ref);
}
