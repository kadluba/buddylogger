/*
 * BuddyLogger - Periodically log states of buddies to a file
 * Christian Kadluba (christian.kadluba@gmail.com) 2011
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02111-1301, USA.
 */


#define PURPLE_PLUGINS
#include <glib.h>
#ifndef G_GNUC_NULL_TERMINATED
	#if __GNUC__ >= 4
		#define G_GNUC_NULL_TERMINATED __attribute__((__sentinel__))
	#else
		#define G_GNUC_NULL_TERMINATED
	#endif /* __GNUC__ >= 4 */
#endif /* G_GNUC_NULL_TERMINATED */
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "notify.h"
#include "plugin.h"
#include "version.h"


/* Constants */

#define PLUGIN_CORE_ID    "/plugins/core/"
#define PLUGIN_ID         "core-buddylogger"
#define PREF_PATH_ID      PLUGIN_CORE_ID PLUGIN_ID "/path"
#define PREF_TRIGGER_ID   PLUGIN_CORE_ID PLUGIN_ID "/trigger"
#define PREF_MINDELAY_ID  PLUGIN_CORE_ID PLUGIN_ID "/mindelay"
#define PREF_CYCLE_ID     PLUGIN_CORE_ID PLUGIN_ID "/cycle"


/* Prototypes */

static time_t get_timestamp(char *buffer, size_t size);
static int get_presence_code(PurpleBlistNode* node);
static void dump_buddies();
static gboolean check_last_write_time(time_t current_time);
static int check_and_lock_file();
static void unlock_file();
static void buddy_status_changed_cb(PurpleBuddy *buddy, void *data);
static void buddy_idle_changed_cb(PurpleBuddy *buddy, void *data);
static void buddy_signon_cb(PurpleBuddy *buddy, void *data);
static void buddy_signoff_cb(PurpleBuddy *buddy, void *data);
static gboolean plugin_load(PurplePlugin *plugin);
static gboolean plugin_unload(PurplePlugin *plugin);
static PurplePluginPrefFrame *get_plugin_pref_frame(PurplePlugin *plugin);
static void init_plugin(PurplePlugin *plugin);


/* Globals */

static PurplePluginUiInfo prefs_info = {
	get_plugin_pref_frame,
	0,
	NULL,
	NULL,
	NULL,
	NULL,
	NULL
};

static PurplePluginInfo info = {
    PURPLE_PLUGIN_MAGIC,
    PURPLE_MAJOR_VERSION,
    PURPLE_MINOR_VERSION,
    PURPLE_PLUGIN_STANDARD,
    NULL,
    0,
    NULL,
    PURPLE_PRIORITY_DEFAULT,

    PLUGIN_ID,
    "BuddyLogger",
    "0.1",

    "BuddyLogger",
    "BuddyLogger",
    "Christian Kadluba",
    "",

    plugin_load,
    plugin_unload,
    NULL,

    NULL,
    NULL,
    &prefs_info,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL
};
PURPLE_INIT_PLUGIN(buddylogger, init_plugin, info)

static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static const char *g_pref_log_file = NULL;
static FILE *g_fp = NULL;
static time_t g_last_write_time = 0;


/* Implementation */

static time_t get_timestamp(char *buffer, size_t size)
{
	time_t time_sec;
	struct tm *p_time;

	time_sec = time(NULL);
	p_time = localtime(&time_sec);	

	strftime(buffer, size - 1, "%Y-%m-%dT%H:%M:%S", p_time);

	return time_sec;
}

static int get_presence_code(PurpleBlistNode* node)
{
	PurpleBuddy *buddy;
	int code = 0;

	if (PURPLE_BLIST_NODE_IS_CONTACT(node))
		node = PURPLE_BLIST_NODE(purple_contact_get_priority_buddy(PURPLE_CONTACT(node)));
	if (!PURPLE_BLIST_NODE_IS_BUDDY(node))
		return 0;

	buddy = (PurpleBuddy*)node;
	if (purple_presence_is_idle(purple_buddy_get_presence(buddy))) {
		code = 2;
	} else if (purple_presence_is_available(purple_buddy_get_presence(buddy))) {
		code = 3;
	} else if (purple_presence_is_online(purple_buddy_get_presence(buddy)) &&
			!purple_presence_is_available(purple_buddy_get_presence(buddy))) {
		code = 1;
	} else if (!purple_presence_is_online(purple_buddy_get_presence(buddy))) {
		code = 0;
	}

	return code;
}

static void dump_buddies()
{
	int file_state = 0;
	PurpleBuddyList *list = NULL;
	time_t current_time = 0;
	char time[50];

	file_state = check_and_lock_file();
	if (file_state == -1) {
		return;
	}

	/* Exit if file not open or last write less than 30 seconds ago */
	current_time = get_timestamp(time, 50);
	if (!check_last_write_time(current_time) && file_state == 1) {
		unlock_file();
		return;
	}

	list = purple_get_blist();
	if (list != NULL) {
		PurpleBlistNode *gnode, *cnode;

		if (file_state == 0) {
			fprintf(g_fp, "Time,");
		} else {
			fprintf(g_fp, "%s,", time);
		}

		for(gnode = list->root; gnode != NULL; gnode = gnode->next) {
			if(PURPLE_BLIST_NODE_IS_GROUP(gnode)) {
				for(cnode = gnode->child; cnode != NULL; cnode = cnode->next) {
					if(PURPLE_BLIST_NODE_IS_CONTACT(cnode)) {
						if (file_state == 0) {
							const char *name;
							name = purple_contact_get_alias((PurpleContact*) cnode);
							if (name != NULL) {
								fprintf(g_fp, "%s", name);
							}
						} else {	
							int presence = get_presence_code(cnode);
							fprintf(g_fp, "%i", presence);
						}
					} else {
						fprintf(g_fp, "nc");						
					}

					if (cnode->next != NULL) {
						fprintf(g_fp, ",");
					}
				}
			}

			if (gnode->next != NULL) {
				fprintf(g_fp, ",");
			}

		}

		g_last_write_time = current_time;
		fprintf(g_fp, "\n");
	}

	unlock_file();
}

static gboolean check_last_write_time(time_t current_time)
{
	time_t exp_time = 0;
	int pref_min_delay = 0;
	gboolean ret = FALSE;

	pref_min_delay = purple_prefs_get_int(PREF_MINDELAY_ID);
	exp_time = g_last_write_time + pref_min_delay;
	if (exp_time < current_time) {
		ret = TRUE;
	}

	return ret;
}

static int check_and_lock_file()
{
	FILE *fp_read = NULL;
	int ret = 0;

	pthread_mutex_lock(&g_mutex);
	g_pref_log_file = purple_prefs_get_string(PREF_PATH_ID);
	if (g_fp != NULL || g_pref_log_file == NULL ) {
		pthread_mutex_unlock(&g_mutex);
		return -1;
	}

	/* Check if logfile exists */
	fp_read = fopen(g_pref_log_file, "r");
	if (fp_read != NULL) {
		fclose(fp_read);
		ret = 1;
	} else {
		ret = 0;
	}

	/* Open logfile for appending */
	g_fp = fopen(g_pref_log_file, "a");
	if (g_fp == NULL) {
		pthread_mutex_unlock(&g_mutex);
		ret = -1;
	}

	return ret;
}

static void unlock_file()
{
	if (g_fp != NULL) {
		fclose(g_fp);
		g_fp = NULL;
	}
	pthread_mutex_unlock(&g_mutex);
}

static void buddy_status_changed_cb(PurpleBuddy *buddy, void *data)
{
	dump_buddies();
}

static void buddy_idle_changed_cb(PurpleBuddy *buddy, void *data)
{
	dump_buddies();
}

static void buddy_signon_cb(PurpleBuddy *buddy, void *data)
{
	dump_buddies();
}

static void buddy_signoff_cb(PurpleBuddy *buddy, void *data)
{
	dump_buddies();
}

static gboolean plugin_load(PurplePlugin *plugin)
{
	void *blist_handle;

	/* Create new log file with header row if it does not exist */
	dump_buddies();

	/* Enable callbacks or start thread for logfile writing */
	if (purple_prefs_get_int(PREF_TRIGGER_ID) == 0) {
		blist_handle = purple_blist_get_handle();
		purple_signal_connect(blist_handle, "buddy-status-changed", plugin,
						PURPLE_CALLBACK(buddy_status_changed_cb), NULL);
		purple_signal_connect(blist_handle, "buddy-idle-changed", plugin,
						PURPLE_CALLBACK(buddy_idle_changed_cb), NULL);
		purple_signal_connect(blist_handle, "buddy-signed-on", plugin,
				PURPLE_CALLBACK(buddy_signon_cb), NULL);
		purple_signal_connect(blist_handle, "buddy-signed-off", plugin,
				PURPLE_CALLBACK(buddy_signoff_cb), NULL);
	} else {
		/* TODO: Start a separate thread for periodic logging */
	}

	return TRUE;
}

static gboolean plugin_unload(PurplePlugin *plugin)
{
	unlock_file();
	return TRUE;
}

static PurplePluginPrefFrame *get_plugin_pref_frame(PurplePlugin *plugin)
{
	PurplePluginPrefFrame *frame;
	PurplePluginPref *pref;

	frame = purple_plugin_pref_frame_new();

	pref = purple_plugin_pref_new_with_name_and_label(PREF_PATH_ID,
			"CSV file name and path");
	purple_plugin_pref_frame_add(frame, pref);

	pref = purple_plugin_pref_new_with_name_and_label(PREF_MINDELAY_ID,
			"Minimum delay between log entries in seconds");
	purple_plugin_pref_set_bounds(pref, 10, 60 * 60 * 3); /* 10 seconds - 3 hours */
	purple_plugin_pref_frame_add(frame, pref);

	/* TODO: read PREF_CYCLE_ID as integer */

	return frame;
}
    
static void init_plugin(PurplePlugin *plugin)
{
	char *log_file_def;

	log_file_def = g_build_filename(purple_home_dir(), "buddylog.csv", NULL);

	purple_prefs_add_none(PLUGIN_CORE_ID PLUGIN_ID);
	purple_prefs_add_string(PREF_PATH_ID, log_file_def);
	purple_prefs_add_int(PREF_MINDELAY_ID, 300);

	g_free(log_file_def);
}

