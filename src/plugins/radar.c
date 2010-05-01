/*
 * Copyright (C) 2009-2010 Andy Spencer <andy753421@gmail.com>
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

#define _XOPEN_SOURCE
#include <time.h>
#include <config.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <gtk/gtkgl.h>
#include <gio/gio.h>
#include <GL/gl.h>
#include <math.h>
#include <rsl.h>

#include <gis.h>

#include "radar.h"
#include "level2.h"
#include "../aweather-location.h"

void _gtk_bin_set_child(GtkBin *bin, GtkWidget *new)
{
	GtkWidget *old = gtk_bin_get_child(bin);
	if (old)
		gtk_widget_destroy(old);
	gtk_container_add(GTK_CONTAINER(bin), new);
	gtk_widget_show_all(new);
}

static gchar *_find_nearest(time_t time, GList *files,
		gsize offset, gchar *format)
{
	g_debug("GisPluginRadar: _find_nearest");
	time_t  nearest_time = 0;
	char   *nearest_file = NULL;

	struct tm tm = {};
	for (GList *cur = files; cur; cur = cur->next) {
		gchar *file = cur->data;
		g_message("file=%s", file);
		strptime(file+offset, format, &tm);
		if (ABS(time - mktime(&tm)) <
		    ABS(time - nearest_time)) {
			nearest_file = file;
			nearest_time = mktime(&tm);
		}
	}

	g_debug("GisPluginRadar: _find_nearest = %s", nearest_file);
	if (nearest_file)
		return g_strdup(nearest_file);
	else
		return NULL;
}


/**************
 * RadarSites *
 **************/
typedef enum {
	STATUS_UNLOADED,
	STATUS_LOADING,
	STATUS_LOADED,
} RadarSiteStatus;
struct _RadarSite {
	/* Information */
	gchar     *code;   // Site name. e.g. KLSX
	gchar     *name;   // Site name. e.g. St. Louis
	GisPoint   pos;    // LLE positions of antena 
	GisMarker *marker; // Map marker for libgis

	/* Stuff from the parents */
	GisViewer     *viewer;
	GisHttp       *http;
	GisPrefs      *prefs;
	GtkWidget     *pconfig;

	/* When loaded */
	RadarSiteStatus status;     // Loading status for the site
	GtkWidget      *config;
	AWeatherLevel2 *level2;     // The Level2 structure for the current volume
	gpointer        level2_ref; // GisViewer reference to the added radar

	/* Internal data */
	time_t   time;        // Current timestamp of the level2
	gchar   *message;     // Error message set while updating
	guint    time_id;     // "time-changed"     callback ID
	guint    refresh_id;  // "refresh"          callback ID
	guint    location_id; // "locaiton-changed" callback ID
};

/* format: http://mesonet.agron.iastate.edu/data/nexrd2/raw/KABR/KABR_20090510_0323 */
void _site_update_loading(gchar *file, goffset cur,
		goffset total, gpointer _site)
{
	RadarSite *site = _site;
	GtkWidget *progress_bar = gtk_bin_get_child(GTK_BIN(site->config));
	double percent = (double)cur/total;
	gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(progress_bar), MIN(percent, 1.0));
	gchar *msg = g_strdup_printf("Loading... %5.1f%% (%.2f/%.2f MB)",
			percent*100, (double)cur/1000000, (double)total/1000000);
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress_bar), msg);
	g_free(msg);
}
gboolean _site_update_end(gpointer _site)
{
	RadarSite *site = _site;
	if (site->message) {
		g_warning("GisPluginRadar: _update_end - %s", site->message);
		_gtk_bin_set_child(GTK_BIN(site->config), gtk_label_new(site->message));
	} else {
		_gtk_bin_set_child(GTK_BIN(site->config),
				aweather_level2_get_config(site->level2));
	}
	site->status = STATUS_LOADED;
	return FALSE;
}
gpointer _site_update_thread(gpointer _site)
{
	RadarSite *site = _site;
	g_debug("GisPluginRadar: _update - %s", site->code);
	site->status = STATUS_LOADING;
	site->message = NULL;

	gboolean offline = gis_viewer_get_offline(site->viewer);
	gchar *nexrad_url = gis_prefs_get_string(site->prefs,
			"aweather/nexrad_url", NULL);

	/* Remove old volume */
	g_debug("GisPluginRadar: _update - remove - %s", site->code);
	if (site->level2_ref) {
		gis_viewer_remove(site->viewer, site->level2_ref);
		site->level2_ref = NULL;
	}

	/* Find nearest volume (temporally) */
	g_debug("GisPluginRadar: _update - find nearest - %s", site->code);
	gchar *dir_list = g_strconcat(nexrad_url, "/", site->code,
			"/", "dir.list", NULL);
	GList *files = gis_http_available(site->http,
			"^K\\w{3}_\\d{8}_\\d{4}$", site->code,
			"\\d+ (.*)", (offline ? NULL : dir_list));
	g_free(dir_list);
	gchar *nearest = _find_nearest(site->time, files, 5, "%Y%m%d_%H%M");
	g_list_foreach(files, (GFunc)g_free, NULL);
	g_list_free(files);
	if (!nearest) {
		site->message = "No suitable files found";
		goto out;
	}

	/* Fetch new volume */
	g_debug("GisPluginRadar: _update - fetch");
	gchar *local = g_strconcat(site->code, "/", nearest, NULL);
	gchar *uri   = g_strconcat(nexrad_url, "/", local,   NULL);
	gchar *file = gis_http_fetch(site->http, uri, local,
			offline ? GIS_LOCAL : GIS_UPDATE,
			_site_update_loading, site);
	g_free(local);
	g_free(uri);
	if (!file) {
		site->message = "Fetch failed";
		goto out;
	}

	/* Load and add new volume */
	g_debug("GisPluginRadar: _update - load - %s", site->code);
	site->level2 = aweather_level2_new_from_file(
			site->viewer, colormaps, file, site->code);
	if (!site->level2) {
		site->message = "Load failed";
		goto out;
	}
	site->level2_ref = gis_viewer_add(site->viewer,
			GIS_OBJECT(site->level2), GIS_LEVEL_WORLD, TRUE);

out:
	g_idle_add(_site_update_end, site);
	return NULL;
}
void _site_update(RadarSite *site)
{
	site->time = gis_viewer_get_time(site->viewer);
	g_debug("GisPluginRadar: _on_time_changed %s - %d",
			site->code, (gint)site->time);

	/* Add a progress bar */
	GtkWidget *progress = gtk_progress_bar_new();
	gtk_progress_bar_set_text(GTK_PROGRESS_BAR(progress), "Loading...");
	_gtk_bin_set_child(GTK_BIN(site->config), progress);

	/* Fork loading right away so updating the
	 * list of times doesn't take too long */
	g_thread_create(_site_update_thread, site, FALSE, NULL);
}

/* RadarSite methods */
void radar_site_unload(RadarSite *site)
{
	g_debug("GisPluginRadar: radar_site_unload %s", site->code);

	if (site->status == STATUS_LOADING)
		return; // Abort if it's still loading

	g_signal_handler_disconnect(site->viewer, site->time_id);
	g_signal_handler_disconnect(site->viewer, site->refresh_id);

	/* Remove tab */
	gtk_widget_destroy(site->config);

	/* Remove radar */
	if (site->level2_ref) {
		gis_viewer_remove(site->viewer, site->level2_ref);
		site->level2_ref = NULL;
	}

	site->status = STATUS_UNLOADED;
}

void radar_site_load(RadarSite *site)
{
	g_debug("GisPluginRadar: radar_site_load %s", site->code);
	site->status = STATUS_LOADING;

	/* Add tab page */
	site->config = gtk_alignment_new(0, 0, 1, 1);
	GtkWidget *tab   = gtk_hbox_new(FALSE, 0);
	GtkWidget *close = gtk_button_new();
	GtkWidget *label = gtk_label_new(site->name);
	gtk_container_add(GTK_CONTAINER(close),
			gtk_image_new_from_stock(GTK_STOCK_CLOSE,
				GTK_ICON_SIZE_MENU));
	gtk_button_set_relief(GTK_BUTTON(close), GTK_RELIEF_NONE);
	g_signal_connect_swapped(close, "clicked",
			G_CALLBACK(radar_site_unload), site);
	gtk_box_pack_start(GTK_BOX(tab), label, TRUE, TRUE, 0);
	gtk_box_pack_end(GTK_BOX(tab), close, FALSE, FALSE, 0);
	gtk_notebook_append_page(GTK_NOTEBOOK(site->pconfig),
			site->config, tab);
	gtk_widget_show_all(site->config);
	gtk_widget_show_all(tab);

	/* Set up radar loading */
	site->time_id = g_signal_connect_swapped(site->viewer, "time-changed",
			G_CALLBACK(_site_update), site);
	site->refresh_id = g_signal_connect_swapped(site->viewer, "refresh",
			G_CALLBACK(_site_update), site);
	_site_update(site);
}

void _site_on_location_changed(GisViewer *viewer,
		gdouble lat, gdouble lon, gdouble elev,
		gpointer _site)
{
	static gdouble min_dist = EARTH_R / 20;
	RadarSite *site = _site;

	/* Calculate distance, could cache xyz values */
	gdouble eye_xyz[3], site_xyz[3];
	lle2xyz(lat, lon, elev, &eye_xyz[0], &eye_xyz[1], &eye_xyz[2]);
	lle2xyz(site->pos.lat, site->pos.lon, site->pos.elev,
			&site_xyz[0], &site_xyz[1], &site_xyz[2]);
	gdouble dist = distd(site_xyz, eye_xyz);

	/* Load or unload the site if necessasairy */
	if (dist <= min_dist && dist < elev*1.25 && site->status == STATUS_UNLOADED)
		radar_site_load(site);
	else if (dist > 2*min_dist &&  site->status != STATUS_UNLOADED)
		radar_site_unload(site);
}

RadarSite *radar_site_new(city_t *city, GtkWidget *pconfig,
		GisViewer *viewer, GisPrefs *prefs, GisHttp *http)
{
	RadarSite *site = g_new0(RadarSite, 1);
	site->viewer  = g_object_ref(viewer);
	site->prefs   = g_object_ref(prefs);
	site->http    = http;
	site->code    = g_strdup(city->code);
	site->name    = g_strdup(city->name);
	site->pos     = city->pos;
	site->pconfig = pconfig;

	/* Add marker */
	site->marker = gis_marker_new(city->name);
	GIS_OBJECT(site->marker)->center = site->pos;
	GIS_OBJECT(site->marker)->lod    = EARTH_R*city->lod;
	gis_viewer_add(site->viewer, GIS_OBJECT(site->marker),
			GIS_LEVEL_OVERLAY, FALSE);

	/* Connect signals */
	site->location_id  =
		g_signal_connect(viewer, "location-changed",
			G_CALLBACK(_site_on_location_changed), site);
	return site;
}

void radar_site_free(RadarSite *site)
{
	radar_site_unload(site);
	/* Stuff? */
	g_object_unref(site->viewer);
	g_free(site->code);
	g_free(site);
}


/******************
 * GisPluginRadar *
 ******************/
static void _draw_hud(GisCallback *callback, gpointer _self)
{
	/* TODO */
	GisPluginRadar *self = GIS_PLUGIN_RADAR(_self);
	if (!self->colormap)
		return;

	g_debug("GisPluginRadar: _draw_hud");
	/* Print the color table */
	glMatrixMode(GL_MODELVIEW ); glLoadIdentity();
	glMatrixMode(GL_PROJECTION); glLoadIdentity();
	glDisable(GL_TEXTURE_2D);
	glDisable(GL_ALPHA_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_LIGHTING);
	glEnable(GL_COLOR_MATERIAL);
	glBegin(GL_QUADS);
	int i;
	for (i = 0; i < 256; i++) {
		glColor4ubv(self->colormap->data[i]);
		glVertex3f(-1.0, (float)((i  ) - 256/2)/(256/2), 0.0); // bot left
		glVertex3f(-1.0, (float)((i+1) - 256/2)/(256/2), 0.0); // top left
		glVertex3f(-0.9, (float)((i+1) - 256/2)/(256/2), 0.0); // top right
		glVertex3f(-0.9, (float)((i  ) - 256/2)/(256/2), 0.0); // bot right
	}
	glEnd();
}

/* Methods */
GisPluginRadar *gis_plugin_radar_new(GisViewer *viewer, GisPrefs *prefs)
{
	/* TODO: move to constructor if possible */
	g_debug("GisPluginRadar: new");
	GisPluginRadar *self = g_object_new(GIS_TYPE_PLUGIN_RADAR, NULL);
	self->viewer = viewer;
	self->prefs  = prefs;

	/* Load HUD */
	GisCallback *hud_cb = gis_callback_new(_draw_hud, self);
	gis_viewer_add(viewer, GIS_OBJECT(hud_cb), GIS_LEVEL_HUD, FALSE);

	/* Load radar sites */
	for (city_t *city = cities; city->type; city++) {
		if (city->type != LOCATION_CITY)
			continue;
		RadarSite *site = radar_site_new(city, self->config,
				self->viewer, self->prefs, self->sites_http);
		g_hash_table_insert(self->sites, city->code, site);
	}

	return self;
}

static GtkWidget *gis_plugin_radar_get_config(GisPlugin *_self)
{
	GisPluginRadar *self = GIS_PLUGIN_RADAR(_self);
	return self->config;
}

/* GObject code */
static void gis_plugin_radar_plugin_init(GisPluginInterface *iface);
G_DEFINE_TYPE_WITH_CODE(GisPluginRadar, gis_plugin_radar, G_TYPE_OBJECT,
		G_IMPLEMENT_INTERFACE(GIS_TYPE_PLUGIN,
			gis_plugin_radar_plugin_init));
static void gis_plugin_radar_plugin_init(GisPluginInterface *iface)
{
	g_debug("GisPluginRadar: plugin_init");
	/* Add methods to the interface */
	iface->get_config = gis_plugin_radar_get_config;
}
static void gis_plugin_radar_init(GisPluginRadar *self)
{
	g_debug("GisPluginRadar: class_init");
	/* Set defaults */
	self->sites_http = gis_http_new(G_DIR_SEPARATOR_S "nexrad" G_DIR_SEPARATOR_S "level2" G_DIR_SEPARATOR_S);
	self->sites      = g_hash_table_new(g_str_hash, g_str_equal);
	self->config     = gtk_notebook_new();
	gtk_notebook_set_tab_pos(GTK_NOTEBOOK(self->config), GTK_POS_LEFT);
}
static void gis_plugin_radar_dispose(GObject *gobject)
{
	g_debug("GisPluginRadar: dispose");
	GisPluginRadar *self = GIS_PLUGIN_RADAR(gobject);
	/* Drop references */
	G_OBJECT_CLASS(gis_plugin_radar_parent_class)->dispose(gobject);
}
static void gis_plugin_radar_finalize(GObject *gobject)
{
	g_debug("GisPluginRadar: finalize");
	GisPluginRadar *self = GIS_PLUGIN_RADAR(gobject);
	/* Free data */
	gis_http_free(self->sites_http);
	g_hash_table_destroy(self->sites);
	gtk_widget_destroy(self->config);
	G_OBJECT_CLASS(gis_plugin_radar_parent_class)->finalize(gobject);

}
static void gis_plugin_radar_class_init(GisPluginRadarClass *klass)
{
	g_debug("GisPluginRadar: class_init");
	GObjectClass *gobject_class = (GObjectClass*)klass;
	gobject_class->dispose  = gis_plugin_radar_dispose;
	gobject_class->finalize = gis_plugin_radar_finalize;
}

