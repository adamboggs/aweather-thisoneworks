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

 /* TODO:
  *    If gpsd connection fails, try to connect again periodically.
  *    If gps stops sending data there should be an indication that it's stale.
  */

#define _XOPEN_SOURCE
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <config.h>
#include <assert.h>
#include <string.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gio/gio.h>
#include <math.h>

#include <grits.h>
#include <gps.h>

#include "gps-plugin.h"
#include "level2.h"
#include "../aweather-location.h"

/* interval to update map with new gps data in seconds. */
#define GPS_UPDATE_INTERVAL (2)

/* interval to update log file in seconds (default value for slider) */
#define GPS_LOG_DEFAULT_UPDATE_INTERVAL (30)
#define GPS_LOG_EXT "csv"

/* For updating the status bar conveniently */
#define GPS_STATUSBAR_CONTEXT "GPS"
#if 0
#define GPS_STATUS(gps_state, format, args...) \
    do { \
        char *buf = g_strdup_printf(format, ##args); \
        gtk_statusbar_push(GTK_STATUSBAR(gps_state->status_bar), \
            gtk_statusbar_get_context_id( \
                GTK_STATUSBAR(gps_state->status_bar), \
                GPS_STATUSBAR_CONTEXT), \
            buf); \
    } while (0)
#endif

#define GPS_STATUS(gps_state, format, args...) \
    do { \
        char *buf = g_strdup_printf(format, ##args); \
	g_warning("STATUS: %s", buf); \
    } while (0)


static gboolean gps_data_is_valid(struct gps_data_t *gps_data);
static char *gps_get_time_string(time_t gps_time);
static char *gps_get_date_string(double gps_time);
static void process_gps( gpointer, gint, GdkInputCondition);

#ifdef GPS_RANGE_RINGS
static void gps_init_range_rings(GritsPluginGPS *gps_state,
            GtkWidget *gbox);
static gboolean on_gps_rangering_clicked_event (GtkWidget *widget, gpointer user_data);
#endif

static void gps_init_status_info(GritsPluginGPS *gps_state,
	    GtkWidget *gbox);
static void gps_init_control_frame(GritsPluginGPS *gps_state,
	    GtkWidget *gbox);
static gboolean on_gps_follow_clicked_event (GtkWidget *widget, gpointer user_data);

/* GPS logging support */
static void gps_init_track_log_frame(GritsPluginGPS *gps_state,
	    GtkWidget *gbox);
static gboolean on_gps_log_clicked_event (GtkWidget *widget, gpointer user_data);
static gboolean on_gps_track_clicked_event (GtkWidget *widget, gpointer user_data);
static gboolean gps_write_log(gpointer data);

static char *gps_get_status(struct gps_data_t *);
static char *gps_get_latitude(struct gps_data_t *);
static char *gps_get_longitude(struct gps_data_t *);
static char *gps_get_elevation(struct gps_data_t *);
static char *gps_get_heading(struct gps_data_t *);
static char *gps_get_speed(struct gps_data_t *);

/* Describes a line in the gps table */
struct gps_status_info {
    char *label;
    char *initial_val;
    char *(*get_data)(struct gps_data_t *);
    unsigned int font_size;
    GtkWidget *label_widget;
    GtkWidget *value_widget;
};

struct gps_status_info gps_table[] = {
    {"Status:", "No Data", gps_get_status, 14, NULL, NULL},
//    {"Online:", "No Data", gps_get_online, 14, NULL, NULL},
    {"Latitude:", "No Data", gps_get_latitude, 14, NULL, NULL},
    {"Longitude:", "No Data", gps_get_longitude, 14, NULL, NULL},
    {"Elevation:", "No Data", gps_get_elevation, 14, NULL, NULL},
    {"Heading:", "No Data", gps_get_heading, 14, NULL, NULL},
    {"Speed:", "No Data", gps_get_speed, 14, NULL, NULL},
};

static
gboolean gps_data_is_valid(struct gps_data_t *gps_data)
{
    if (gps_data != NULL && gps_data->online != -1.0 &&
	gps_data->fix.mode >= MODE_2D &&
	gps_data->status > STATUS_NO_FIX) {
	return TRUE;
    }

    return FALSE;
}

static char *
gps_get_date_string(double gps_time)
{
    static char	buf[256];
    time_t 	int_time = (time_t)gps_time;
    struct tm	tm_time;

    gmtime_r(&int_time, &tm_time);

    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
		 tm_time.tm_year+1900, tm_time.tm_mon+1, tm_time.tm_mday);


    return buf;
}

static char *
gps_get_time_string(time_t gps_time)
{
    static char buf[256];
    time_t 	int_time = (time_t)gps_time;
    struct tm	tm_time;

    gmtime_r(&int_time, &tm_time);

    snprintf(buf, sizeof(buf), "%02d:%02d:%02dZ",
		 tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);

    return buf;
}


static void
update_gps_status(GritsPluginGPS *gps_state)
{
    struct gps_data_t *gps_data = &gps_state->gps_data;

    /* gps table update */
    int i;
    gchar *str;
    for (i = 0; i < sizeof(gps_table)/sizeof(*gps_table); i++) {
	gtk_label_set_markup (GTK_LABEL(gps_table[i].value_widget),
		(str = gps_table[i].get_data(gps_data)));
	g_free(str);
    }
}

static void
gps_init_control_frame(GritsPluginGPS *gps_state, GtkWidget *gbox)
{
    /* Control checkboxes */
    GtkWidget *gps_control_frame = gtk_frame_new ("GPS Control");
    GtkWidget *cbox = gtk_vbox_new (FALSE, 2);
    gtk_container_add (GTK_CONTAINER (gps_control_frame), cbox);
    gtk_box_pack_start (GTK_BOX(gbox), gps_control_frame, FALSE, FALSE, 0);

    gps_state->ui.gps_follow_checkbox = gtk_check_button_new_with_label("Follow GPS");
    g_signal_connect (G_OBJECT (gps_state->ui.gps_follow_checkbox), "clicked",
                      G_CALLBACK (on_gps_follow_clicked_event),
		      (gpointer)gps_state);
    gtk_box_pack_start (GTK_BOX(cbox), gps_state->ui.gps_follow_checkbox,
			FALSE, FALSE, 0);
    gps_state->ui.gps_track_checkbox = gtk_check_button_new_with_label("Show Track");
    g_signal_connect (G_OBJECT (gps_state->ui.gps_track_checkbox), "clicked",
                      G_CALLBACK (on_gps_track_clicked_event),
		      (gpointer)gps_state);
    gtk_box_pack_start (GTK_BOX(cbox), gps_state->ui.gps_track_checkbox,
			FALSE, FALSE, 0);
}

static gboolean
on_gps_track_clicked_event (GtkWidget *widget, gpointer user_data)
{
    g_debug("on_gps_track_clicked_event called!");
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget))) {
	/* XXX start logging trip history */
	GPS_STATUS(gps_state, "Enabled GPS track.");
    } else {
	/* XXX stop logging trip history */
	GPS_STATUS(gps_state, "Disabled GPS track.");
    }

    return FALSE;
}

static gboolean
on_gps_log_interval_changed_event(GtkWidget *widget, gpointer user_data)
{
    GritsPluginGPS *gps_state = (GritsPluginGPS *)user_data;

    assert(gps_state);

    g_debug("gps interval changed, value = %f",
	gtk_range_get_value(GTK_RANGE(widget)));

    if (gtk_toggle_button_get_active(
			GTK_TOGGLE_BUTTON(gps_state->ui.gps_log_checkbox))) {
	assert(gps_state->ui.gps_log_timeout_id != 0);

	/* disable old timeout */
	g_source_remove(gps_state->ui.gps_log_timeout_id);
	gps_state->ui.gps_log_timeout_id = 0;

	/* Schedule new log file write */
	gps_state->ui.gps_log_timeout_id = g_timeout_add(
				gtk_range_get_value(GTK_RANGE(widget))*1000,
				gps_write_log, gps_state);
	gps_write_log(gps_state);
    }

    return FALSE;
}

static gboolean
gps_write_log(gpointer data)
{
    GritsPluginGPS *gps_state = (GritsPluginGPS *)data;
    struct gps_data_t *gps_data = &gps_state->gps_data;
    char buf[256];
    char filename[256];
    int fd;
    gboolean new_file = FALSE;

    if (gps_data == NULL) {
        g_warning("Skipped write to GPS log file: "
                "can not get GPS coordinates.");
        GPS_STATUS(gps_state, "Skipped write to GPS log file: "
                "can not get GPS coordinates.");
        return TRUE;
    }

    /* get filename from text entry box.  If empty, generate a name from
     * the date and time and set it.
     */
    if (strlen(gtk_entry_get_text(GTK_ENTRY(gps_state->ui.gps_log_filename_entry)))
								    == 0) {
	snprintf(filename, sizeof(filename),
			    "%sT%s.%s",
			    gps_get_date_string(gps_state->gps_data.fix.time),
			    gps_get_time_string(gps_state->gps_data.fix.time),
			    GPS_LOG_EXT);
	gtk_entry_set_text(GTK_ENTRY(gps_state->ui.gps_log_filename_entry),
			    filename);
    }

    strncpy(filename,
	    gtk_entry_get_text(GTK_ENTRY(gps_state->ui.gps_log_filename_entry)),
	    sizeof (filename));

    if (!g_file_test(filename, G_FILE_TEST_EXISTS)) {
	new_file = TRUE;
    }

    if ((fd = open(filename, O_CREAT|O_APPEND|O_WRONLY, 0644)) == -1) {
	g_warning("Error opening log file %s: %s",
			filename, strerror(errno));
	return FALSE;
    }

    if (new_file) {
	/* write header and reset record counter */
	snprintf(buf, sizeof(buf),
		"No,Date,Time,Lat,Lon,Ele,Head,Speed,RTR\n");
	if (write(fd, buf, strlen(buf)) == -1) {
	    g_warning("Error writing header to log file %s: %s",
			    filename, strerror(errno));
	}
	gps_state->ui.gps_log_number = 1;
    }

    /* Write log entry.  Make sure this matches the header */
    /* "No,Date,Time,Lat,Lon,Ele,Head,Speed,Fix,RTR\n" */
    /* RTR values: T=time, B=button push, S=speed, D=distance */
    snprintf(buf, sizeof(buf), "%d,%s,%s,%lf,%lf,%lf,%lf,%lf,%c\n",
	gps_state->ui.gps_log_number++,
	gps_get_date_string(gps_state->gps_data.fix.time),
	gps_get_time_string(gps_state->gps_data.fix.time),
//	gps_data->fix.time,
	gps_data->fix.latitude,
	gps_data->fix.longitude,
	gps_data->fix.altitude * METERS_TO_FEET,
	gps_data->fix.track,
	gps_data->fix.speed * METERS_TO_FEET,
	'T'); /* position due to timer expired  */

    if (write(fd, buf, strlen(buf)) == -1) {
	g_warning("Could not write log number %d to log file %s: %s",
	    gps_state->ui.gps_log_number-1, filename, strerror(errno));
    }
    close(fd);

    GPS_STATUS(gps_state, "Updated GPS log file %s.", filename);

    /* reschedule */
    return TRUE;
}

static gboolean
on_gps_follow_clicked_event (GtkWidget *widget, gpointer user_data)
{
    GritsPluginGPS *gps_state = (GritsPluginGPS *)user_data;

    g_debug("on_gps_follow_clicked_event called!, button status %d",
	gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)));
    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))
	gps_state->follow_gps = TRUE;
    else
	gps_state->follow_gps = FALSE;

    return FALSE;
}


static void
gps_init_track_log_frame(GritsPluginGPS *gps_state, GtkWidget *gbox)
{
    /* Track log box with enable checkbox and filename entry */
    GtkWidget *gps_log_frame = gtk_frame_new ("Track Log");
    GtkWidget *lbox = gtk_vbox_new (FALSE, 2);
    gtk_container_add (GTK_CONTAINER (gps_log_frame), lbox);
    gtk_box_pack_start (GTK_BOX(gbox), gps_log_frame,
    			FALSE, FALSE, 0);

    gps_state->ui.gps_log_checkbox = gtk_check_button_new_with_label("Log Position to File");
    g_signal_connect (G_OBJECT (gps_state->ui.gps_log_checkbox), "clicked",
                      G_CALLBACK (on_gps_log_clicked_event),
		      (gpointer)gps_state);
    gtk_box_pack_start (GTK_BOX(lbox), gps_state->ui.gps_log_checkbox,
    			FALSE, FALSE, 0);

    /* Set up filename entry box */
    GtkWidget *fbox = gtk_hbox_new (FALSE, 2);
    GtkWidget *filename_label = gtk_label_new ("Filename:");
    gtk_box_pack_start (GTK_BOX(fbox), filename_label, FALSE, FALSE, 0);
    gps_state->ui.gps_log_filename_entry = gtk_entry_new();
    gtk_box_pack_start (GTK_BOX(fbox), gps_state->ui.gps_log_filename_entry,
			TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX(lbox), fbox, FALSE, FALSE, 0);

    /* set up gps log interval slider */
    GtkWidget *ubox = gtk_hbox_new (FALSE, 4);
    GtkWidget *interval_label = gtk_label_new ("Update Interval:");
    gtk_box_pack_start (GTK_BOX(ubox), interval_label, FALSE, FALSE, 0);
    gps_state->ui.gps_log_interval_slider =
		    gtk_hscale_new_with_range(1.0, 600.0, 30.0);
    gtk_range_set_value (GTK_RANGE(gps_state->ui.gps_log_interval_slider),
		    GPS_LOG_DEFAULT_UPDATE_INTERVAL);
    g_signal_connect (G_OBJECT (gps_state->ui.gps_log_interval_slider),
		    "value-changed",
		    G_CALLBACK(on_gps_log_interval_changed_event),
		    (gpointer)gps_state);
    gtk_range_set_increments (GTK_RANGE(gps_state->ui.gps_log_interval_slider),
		    10.0 /* step */, 30.0 /* page up/down */);
    gtk_range_set_update_policy (GTK_RANGE(gps_state->ui.gps_log_interval_slider),
		    GTK_UPDATE_DELAYED);
    gtk_box_pack_start (GTK_BOX(ubox), gps_state->ui.gps_log_interval_slider,
		    TRUE, TRUE, 0);
    gtk_box_pack_start (GTK_BOX(lbox), ubox, FALSE, FALSE, 0);
}

static gboolean
on_gps_log_clicked_event (GtkWidget *widget, gpointer user_data)
{
    GritsPluginGPS *gps_state = (GritsPluginGPS *)user_data;

    g_debug("on_gps_log_clicked_event called!");

    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))  {
	gps_write_log(gps_state);

	/* Schedule log file write */
	gps_state->ui.gps_log_timeout_id = g_timeout_add(
		    gtk_range_get_value(
			GTK_RANGE(gps_state->ui.gps_log_interval_slider))*1000,
			gps_write_log, gps_state);
    } else {
	/* button unchecked */
	g_source_remove(gps_state->ui.gps_log_timeout_id);
	gps_state->ui.gps_log_timeout_id = 0;
	g_debug("Closed log file.");
    }

    return FALSE;
}


static void
gps_init_status_info(GritsPluginGPS *gps_state, GtkWidget *gbox)
{
    gps_state->ui.gps_status_frame = gtk_frame_new ("GPS Data");
    gps_state->ui.gps_status_table = gtk_table_new (5, 2, TRUE);
    gtk_container_add (GTK_CONTAINER (gps_state->ui.gps_status_frame),
    			gps_state->ui.gps_status_table);

    /* gps data table setup */
    int i;
    for (i = 0; i < sizeof(gps_table)/sizeof(*gps_table); i++) {
	gps_table[i].label_widget = gtk_label_new (gps_table[i].label);
	gtk_label_set_justify(GTK_LABEL(gps_table[i].label_widget),
					GTK_JUSTIFY_LEFT);
	gtk_table_attach( GTK_TABLE(gps_state->ui.gps_status_table),
			gps_table[i].label_widget, 0, 1, i, i+1, 0, 0, 0, 0);
	gps_table[i].value_widget = gtk_label_new (gps_table[i].initial_val);
	gtk_table_attach( GTK_TABLE(gps_state->ui.gps_status_table),
			gps_table[i].value_widget, 1, 2, i, i+1, 0, 0, 0, 0);

	PangoFontDescription *font_desc = pango_font_description_new ();
	pango_font_description_set_size (font_desc,
			gps_table[i].font_size*PANGO_SCALE);
	gtk_widget_modify_font (gps_table[i].label_widget, font_desc);
	gtk_widget_modify_font (gps_table[i].value_widget, font_desc);
	pango_font_description_free (font_desc);
    }
    gtk_box_pack_start (GTK_BOX(gbox), gps_state->ui.gps_status_frame,
    			FALSE, FALSE, 0);

    /* Start UI refresh task, which will reschedule itself periodically. */
    gps_redraw_all(gps_state);
    gps_state->gps_update_timeout_id = g_timeout_add(
		    GPS_UPDATE_INTERVAL*1000,
		    gps_redraw_all, gps_state);

}


#ifdef GPS_RANGE_RINGS
static void
gps_init_range_rings(GritsPluginGPS *gps_state, GtkWidget *gbox)
{
    GtkWidget *gps_range_ring_frame = gtk_frame_new ("Range Rings");
    GtkWidget *cbox = gtk_vbox_new (FALSE, 2);
    gtk_container_add (GTK_CONTAINER (gps_range_ring_frame), cbox);
    gtk_box_pack_start (GTK_BOX(gbox), gps_range_ring_frame, FALSE, FALSE, 0);

    gps_state->ui.gps_rangering_checkbox = gtk_check_button_new_with_label("Enable Range Rings");
    g_signal_connect (G_OBJECT (gps_state->ui.gps_rangering_checkbox), "clicked",
                      G_CALLBACK (on_gps_rangering_clicked_event),
		      (gpointer)gps_state);
    gtk_box_pack_start (GTK_BOX(cbox), gps_state->ui.gps_rangering_checkbox,
    			FALSE, FALSE, 0);
}

static gboolean
on_gps_rangering_clicked_event (GtkWidget *widget, gpointer user_data)
{
    GritsPluginGPS *gps_state = (GritsPluginGPS *)user_data;

    if (gtk_toggle_button_get_active (GTK_TOGGLE_BUTTON (widget)))  {
	gps_state->gps_rangering_active = TRUE;
    } else {
	gps_state->gps_rangering_active = FALSE;
    }

    /* XXX force a redraw */

    return FALSE;
}
#endif /* GPS_RANGE_RINGS */

/* external interface to update UI from latest GPS data. */
gboolean gps_redraw_all(gpointer data)
{
    GritsPluginGPS *gps_state = (GritsPluginGPS *)data;
    assert(gps_state);

    struct gps_data_t *gps_data = &gps_state->gps_data;

    g_debug("gps_redraw_all called");

    assert(gps_data);
    if (!gps_data_is_valid(gps_data)) {
        g_debug("gps_data is not valid.");
	/* XXX Change marker to indicate data is not valid */
        return TRUE;
    }

    /* update position labels */
    update_gps_status(gps_state);

    /* Update track and marker position */
    if (gps_data_is_valid(gps_data)) {
        g_debug("Updating track at lat = %f, long = %f, track = %f",
                        gps_data->fix.latitude,
                        gps_data->fix.longitude,
                        gps_data->fix.track);

	if (gps_state->marker) {
	    grits_viewer_remove(gps_state->viewer,
		    GRITS_OBJECT(gps_state->marker));
	    gps_state->marker = NULL;
	}

	gps_state->marker = grits_marker_icon_new("GPS", "car.png",
		gps_data->fix.track, TRUE);
		
        GRITS_OBJECT(gps_state->marker)->center.lat  = gps_data->fix.latitude;
        GRITS_OBJECT(gps_state->marker)->center.lon  = gps_data->fix.longitude;
        GRITS_OBJECT(gps_state->marker)->center.elev =   0.0;
        GRITS_OBJECT(gps_state->marker)->lod         = EARTH_R;

        grits_viewer_add(gps_state->viewer, GRITS_OBJECT(gps_state->marker),
			GRITS_LEVEL_OVERLAY, TRUE);
	grits_viewer_refresh(gps_state->viewer);
    }

    if (gps_state->follow_gps && gps_data_is_valid(gps_data)) {
        /* Center map at current GPS position. */
        g_debug("Centering map at lat = %f, long = %f, track = %f",
                        gps_data->fix.latitude,
                        gps_data->fix.longitude,
                        gps_data->fix.track);

	double lat, lon, elev;
	grits_viewer_get_location(gps_state->viewer, &lat, &lon, &elev);
	grits_viewer_set_location(gps_state->viewer, gps_data->fix.latitude,
					  gps_data->fix.longitude, elev);
	//grits_viewer_set_rotation(gps_state->viewer, 0, 0, 0);
    }

    /* reschedule */
    return TRUE;
}

static
char *gps_get_status(struct gps_data_t *gps_data)
{
    gchar *status_color;
    gchar *status_text;

    switch (gps_data->fix.mode) {
	case MODE_NOT_SEEN:
	    status_color = "red";
	    status_text = "No Signal";
	    break;
	case MODE_NO_FIX:
	    status_color = "red";
	    status_text = "No Fix";
	    break;
	case MODE_2D:
	    status_color = "yellow";
	    status_text = "2D Mode";
	    break;
	case MODE_3D:
	    status_color = "green";
	    status_text = "3D Mode";
	    break;
	default:
	    status_color = "black";
	    status_text = "Unknown";
	    break;
    }
    return g_strdup_printf("<span foreground=\"%s\">%s</span>",
    				status_color, status_text);
}

#if 0
static char *gps_get_online(struct gps_data_t *);

static
char *gps_get_online(struct gps_data_t *gps_data)
{
    char *status_str;
    char *online_str;

    if (gps_data->online == -1.0) {
	online_str = "Offline";
    } else {
	online_str = "Online";
    }

    switch (gps_data->status) {
    case 0:
	status_str = "No Fix";
	break;
    case 1:
	status_str = "Fix Acquired";
	break;
    case 2:
	status_str = "DGPS Fix";
	break;
    default:
	status_str = "Unknown Status";
	break;
    }

    return g_strdup_printf("%lf,%s,%s", gps_data->online, online_str, status_str);
}
#endif



static
char *gps_get_latitude(struct gps_data_t *gps_data)
{
    return g_strdup_printf("%3.4f", gps_data->fix.latitude);
}

static
char *gps_get_longitude(struct gps_data_t *gps_data)
{
    return g_strdup_printf("%3.4f", gps_data->fix.longitude);
}

static
char *gps_get_elevation(struct gps_data_t *gps_data)
{
    /* XXX Make units (m/ft) settable */
    return g_strdup_printf("%.1lf %s",
		    (gps_data->fix.altitude * METERS_TO_FEET), "ft");
}

static
char *gps_get_heading(struct gps_data_t *gps_data)
{
    /* XXX Make units (m/ft) settable */
    return g_strdup_printf("%03.0lf", gps_data->fix.track);
}

static
char *gps_get_speed(struct gps_data_t *gps_data)
{
    /* XXX Make units (m/ft) settable */
    return g_strdup_printf("%1.1f %s",
        (gps_data->fix.speed*3600.0*METERS_TO_FEET/5280.0), "mph");
}


static
gint initialize_gpsd(char *server, char *port,
	struct gps_data_t *gps_data)
{
#if GPSD_API_MAJOR_VERSION < 5
#error "GPSD protocol version 5 or greater required."
#endif
    int result;

    if ((result = gps_open(server, port, gps_data)) != 0) {
	g_warning("Unable to open gpsd connection to %s:%s: %d, %d, %s",
	server, port, result, errno, gps_errstr(errno));
    } else {
        (void)gps_stream(gps_data, WATCH_ENABLE|WATCH_JSON, NULL);
	g_debug("initialize_gpsd(): gpsd fd %u.", gps_data->gps_fd);
        gdk_input_add(gps_data->gps_fd, GDK_INPUT_READ, process_gps, gps_data);
    }

    return result;
}

static void
process_gps(gpointer data, gint source, GdkInputCondition condition)
{
    struct gps_data_t *gps_data = (struct gps_data_t *)data;

    /* Process any data from the gps and call the hook function */
    g_debug("In process_gps()");
    if (gps_data != NULL) {
	int result = gps_read(gps_data);
        g_debug("In process_gps(), gps_read returned %d, position %f, %f.", result, gps_data->fix.latitude, gps_data->fix.longitude);
    } else {
        g_debug("process_gps: gps_data == NULL.");
    }
}

/************************** GPS Object Methods *************************/

/* Methods */
GritsPluginGPS *grits_plugin_gps_new(GritsViewer *viewer, GritsPrefs *prefs)
{
	/* TODO: move to constructor if possible */
	g_debug("GritsPluginGPS: new");
	GritsPluginGPS *self = g_object_new(GRITS_TYPE_PLUGIN_GPS, NULL);
	self->viewer = viewer;
	self->prefs  = prefs;

	g_debug("grits_plugin_gps_new()");

	initialize_gpsd("localhost", DEFAULT_GPSD_PORT, &self->gps_data);
	self->follow_gps = FALSE;

	gps_init_status_info(self, self->hbox);
	gps_init_control_frame(self, self->hbox);
	gps_init_track_log_frame(self, self->hbox);
#ifdef GPS_RANGE_RINGS
	gps_init_range_rings(self, self->hbox);
#endif

	return self;
}

static GtkWidget *grits_plugin_gps_get_config(GritsPlugin *_self)
{
	GritsPluginGPS *self = GRITS_PLUGIN_GPS(_self);
	return self->config;
}

/* GObject code */
static void grits_plugin_gps_plugin_init(GritsPluginInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GritsPluginGPS, grits_plugin_gps, G_TYPE_OBJECT,
		G_IMPLEMENT_INTERFACE(GRITS_TYPE_PLUGIN,
			grits_plugin_gps_plugin_init));

static void grits_plugin_gps_plugin_init(GritsPluginInterface *iface)
{
	g_debug("GritsPluginGPS: plugin_init");
	/* Add methods to the interface */
	iface->get_config = grits_plugin_gps_get_config;
}

static void grits_plugin_gps_init(GritsPluginGPS *self)
{
	g_debug("GritsPluginGPS: in gps_init()");

	self->config     = gtk_notebook_new();

	 self->hbox = gtk_hbox_new(FALSE, 2);
	 gtk_notebook_insert_page(GTK_NOTEBOOK(self->config),
				GTK_WIDGET(self->hbox),
				gtk_label_new("GPS"), 0);
	/* Need to position on the top because of Win32 bug */
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(self->config), GTK_POS_LEFT);
}

static void grits_plugin_gps_dispose(GObject *gobject)
{
	GritsPluginGPS *self = GRITS_PLUGIN_GPS(gobject);

	g_debug("GritsPluginGPS: dispose");

        if (self->viewer) {
		if (self->marker) {
		    grits_viewer_remove(self->viewer,
		    			GRITS_OBJECT(self->marker));
		}
                g_object_unref(self->viewer);
                self->viewer = NULL;
        }

	/* Drop references */
	G_OBJECT_CLASS(grits_plugin_gps_parent_class)->dispose(gobject);
}

static void grits_plugin_gps_finalize(GObject *gobject)
{
	GritsPluginGPS *self = GRITS_PLUGIN_GPS(gobject);

	g_debug("GritsPluginGPS: finalize");

	/* Free data */
	gtk_widget_destroy(self->config);
	G_OBJECT_CLASS(grits_plugin_gps_parent_class)->finalize(gobject);
}

static void grits_plugin_gps_class_init(GritsPluginGPSClass *klass)
{
	g_debug("GritsPluginGPS: class_init");
	GObjectClass *gobject_class = (GObjectClass*)klass;
	gobject_class->dispose  = grits_plugin_gps_dispose;
	gobject_class->finalize = grits_plugin_gps_finalize;
}
