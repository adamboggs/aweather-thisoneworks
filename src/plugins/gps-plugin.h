/*
 * Copyright (C) 2012 Adam Boggs <boggs@aircrafter.org>
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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
#ifndef _GPS_PLUGIN_H
#define _GPS_PLUGIN_H

gpointer gps_init(GtkWidget *gbox, GtkWidget *status_bar);

void gps_set_follow(gpointer state, gboolean track);
gboolean gps_key_press_event(gpointer state, GdkEventKey *kevent);
gboolean gps_redraw_all(gpointer data);

#define GRITS_TYPE_PLUGIN_GPS            (grits_plugin_gps_get_type ())
#define GRITS_PLUGIN_GPS(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),   GRITS_TYPE_PLUGIN_GPS, GritsPluginGPS))
#define GRITS_IS_PLUGIN_GPS(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),   GRITS_TYPE_PLUGIN_GPS))
#define GRITS_PLUGIN_GPS_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST   ((klass), GRITS_TYPE_PLUGIN_GPS, GritsPluginGPSClass))
#define GRITS_IS_PLUGIN_GPS_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE   ((klass), GRITS_TYPE_PLUGIN_GPS))
#define GRITS_PLUGIN_GPS_GET_CLASS(obj)  (G_TYPE_INSTANCE_GET_CLASS ((obj),   GRITS_TYPE_PLUGIN_GPS, GritsPluginGPSClass))

typedef struct _GritsPluginGPS      GritsPluginGPS;
typedef struct _GritsPluginGPSClass GritsPluginGPSClass;

/* All the User Interface objects we need to keep track of. */
struct gps_ui_t {
    /* gps info frame */
    GtkWidget *gps_status_frame;
    GtkWidget *gps_status_table;
    GtkWidget *gps_status_label;
    GtkWidget *gps_latitude_label;
    GtkWidget *gps_longitude_label;
    GtkWidget *gps_heading_label;
    GtkWidget *gps_elevation_label;

    GtkWidget *status_bar;

    /* control frame */
    GtkWidget *gps_follow_checkbox;
    GtkWidget *gps_track_checkbox;

    /* log frame */
    GtkWidget *gps_log_checkbox;
    GtkWidget *gps_log_filename_entry;
    GtkWidget *gps_log_interval_slider;
    guint gps_log_timeout_id;		/* id of timeout so we can delete it */
    unsigned int gps_log_number;	/* sequential log number */

    /* spotternetwork frame */
    GtkWidget *gps_sn_checkbox;
    GtkWidget *gps_sn_active_checkbox;
    gboolean gps_sn_active;		/* Whether chaser is "active" or not */
    SoupSession *soup_session;		/* for sn requests */
    guint gps_sn_timeout_id;		/* id of timeout so we can delete it */
    GtkWidget *gps_sn_entry;		/* Entry box for spotternetwork ID */

    /* range ring frame */
    GtkWidget *gps_rangering_checkbox;
};

/* GPS private data */
struct _GritsPluginGPS {
	GObject parent_instance;

	/* instance members */
	GritsViewer *viewer;
	GritsPrefs  *prefs;
	GtkWidget   *config;
	guint        tab_id;
	GtkWidget *hbox;
	GritsMarker *marker;

	struct gps_data_t *gps_data;
	gboolean follow_gps;
	gboolean gps_sn_active;
	gboolean gps_rangering_active;	/* range rings are visible or not */
	guint gps_update_timeout_id;	/* id of timeout so we can delete it */

	struct gps_ui_t ui;
};

struct _GritsPluginGPSClass {
	GObjectClass parent_class;
};

GType grits_plugin_gps_get_type();


#endif /* GPS_PLUGIN_H */

