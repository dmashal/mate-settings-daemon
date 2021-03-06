/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2001 Udaltsoft
 *
 * Written by Sergey V. Oudaltsov <svu@users.sourceforge.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "config.h"

#include <string.h>
#include <time.h>

#include <glib/gi18n.h>
#include <gdk/gdk.h>
#include <gdk/gdkx.h>
#include <gtk/gtk.h>
#include <gio/gio.h>

#include <libmatekbd/matekbd-status.h>
#include <libmatekbd/matekbd-keyboard-drawing.h>
#include <libmatekbd/matekbd-desktop-config.h>
#include <libmatekbd/matekbd-keyboard-config.h>
#include <libmatekbd/matekbd-util.h>

#include "msd-xmodmap.h"
#include "msd-keyboard-xkb.h"
#include "delayed-dialog.h"
#include "mate-settings-profile.h"

#define GTK_RESPONSE_PRINT 2

#define MATEKBD_DESKTOP_SCHEMA "org.mate.peripherals-keyboard-xkb.general"
#define MATEKBD_KBD_SCHEMA "org.mate.peripherals-keyboard-xkb.kbd"

#define KNOWN_FILES_KEY "known-file-list"

static MsdKeyboardManager* manager = NULL;

static GSettings* settings_desktop;
static GSettings* settings_kbd;

static XklEngine* xkl_engine;
static XklConfigRegistry* xkl_registry = NULL;

static MatekbdDesktopConfig current_config;
static MatekbdKeyboardConfig current_kbd_config;

/* never terminated */
static MatekbdKeyboardConfig initial_sys_kbd_config;

static gboolean inited_ok = FALSE;

static PostActivationCallback pa_callback = NULL;
static void *pa_callback_user_data = NULL;

static GtkStatusIcon* icon = NULL;

static GHashTable* preview_dialogs = NULL;

static Atom caps_lock;
static Atom num_lock;
static Atom scroll_lock;

static GtkStatusIcon* indicator_icons[3];
static const gchar* indicator_on_icon_names[] = {
	"kbd-scrolllock-on",
	"kbd-numlock-on",
	"kbd-capslock-on"
};

static const gchar* indicator_off_icon_names[] = {
	"kbd-scrolllock-off",
	"kbd-numlock-off",
	"kbd-capslock-off"
};

//#define noMSDKX

#ifdef MSDKX
static FILE *logfile;

static void msd_keyboard_log_appender(const char file[], const char function[], int level, const char format[], va_list args)
{
	time_t now = time (NULL);
	fprintf (logfile, "[%08ld,%03d,%s:%s/] \t", now,
		 level, file, function);
	vfprintf (logfile, format, args);
	fflush (logfile);
}
#endif

static void g_strv_delete_str (gchar **a, gchar *str)
{
	int i;
	int j;
	gchar **b;
	b = g_new0 (gchar *, g_strv_length (a) - 1);

	j = 0;
	for (i = 0; a[i] != NULL; i++) {
		if (g_strcmp0 (a[i], str) != 0) {
			b[j] = g_strdup (a[i]);
			j++;
		}
	}
	g_strfreev (a);
	a = b;
}

static void
activation_error (void)
{
	char const *vendor = ServerVendor (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()));
	int release = VendorRelease (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()));
	GtkWidget *dialog;
	gboolean badXFree430Release;

	badXFree430Release = (vendor != NULL)
	    && (0 == strcmp (vendor, "The XFree86 Project, Inc"))
	    && (release / 100000 == 403);

	/* VNC viewers will not work, do not barrage them with warnings */
	if (NULL != vendor && NULL != strstr (vendor, "VNC"))
		return;

	dialog = gtk_message_dialog_new_with_markup (NULL,
						     0,
						     GTK_MESSAGE_ERROR,
						     GTK_BUTTONS_CLOSE,
						     _
						     ("Error activating XKB configuration.\n"
						      "It can happen under various circumstances:\n"
						      " • a bug in libxklavier library\n"
						      " • a bug in X server (xkbcomp, xmodmap utilities)\n"
						      " • X server with incompatible libxkbfile implementation\n\n"
						      "X server version data:\n%s\n%d\n%s\n"
						      "If you report this situation as a bug, please include:\n"
						      " • The result of <b>%s</b>\n"
						      " • The result of <b>%s</b>"),
						     vendor,
						     release,
						     badXFree430Release
						     ?
						     _
						     ("You are using XFree 4.3.0.\n"
						      "There are known problems with complex XKB configurations.\n"
						      "Try using a simpler configuration or using a later version of the XFree software.")
						     : "",
						     "xprop -root | grep XKB",
						     "gsettings list-keys org.mate.peripherals-keyboard-xkb.kbd");
	g_signal_connect (dialog, "response",
			  G_CALLBACK (gtk_widget_destroy), NULL);
	msd_delayed_show_dialog (dialog);
}

static void
apply_desktop_settings (void)
{
	gboolean show_leds;
	int i;
	if (!inited_ok)
		return;

	msd_keyboard_manager_apply_settings (manager);
	matekbd_desktop_config_load_from_gsettings (&current_config);
	/* again, probably it would be nice to compare things
	   before activating them */
	matekbd_desktop_config_activate (&current_config);

	/* FIXME add an option to GSettings to duplicate leds? */
	show_leds = FALSE;
	for (i = sizeof (indicator_icons) / sizeof (indicator_icons[0]);
	     --i >= 0;) {
		gtk_status_icon_set_visible (indicator_icons[i],
					     show_leds);
	}
}

static void
apply_desktop_settings_cb (GSettings *settings, gchar *key, gpointer   user_data)
{
    apply_desktop_settings ();
}

static void
popup_menu_launch_capplet ()
{
	GError *error = NULL;

	gdk_spawn_command_line_on_screen (gdk_screen_get_default (),
					  "mate-keyboard-properties",
					  &error);

	if (error != NULL) {
		g_warning
		    ("Could not execute keyboard properties capplet: [%s]\n",
		     error->message);
		g_error_free (error);
	}
}

static void
show_layout_destroy (GtkWidget * dialog, gint group)
{
	g_hash_table_remove (preview_dialogs, GINT_TO_POINTER (group));
}

static void
popup_menu_show_layout ()
{
	GtkWidget *dialog;
	XklEngine *engine = xkl_engine_get_instance (GDK_DISPLAY_XDISPLAY(gdk_display_get_default()));
	XklState *xkl_state = xkl_engine_get_current_state (engine);
	gpointer p = g_hash_table_lookup (preview_dialogs,
					  GINT_TO_POINTER
					  (xkl_state->group));
	gchar **group_names = matekbd_status_get_group_names ();

	if (xkl_state->group < 0
	    || xkl_state->group >= g_strv_length (group_names)) {
		return;
	}

	if (p != NULL) {
		/* existing window */
		gtk_window_present (GTK_WINDOW (p));
		return;
	}

	dialog =
	    matekbd_keyboard_drawing_new_dialog (xkl_state->group,
					      group_names
					      [xkl_state->group]);
	g_signal_connect (GTK_OBJECT (dialog), "destroy",
			  G_CALLBACK (show_layout_destroy),
			  GINT_TO_POINTER (xkl_state->group));
	g_hash_table_insert (preview_dialogs,
			     GINT_TO_POINTER (xkl_state->group), dialog);
}

static void
popup_menu_set_group (GtkMenuItem * item, gpointer param)
{
	gint group_number = GPOINTER_TO_INT (param);
	XklEngine *engine = matekbd_status_get_xkl_engine ();
	XklState st;
	Window cur;

	st.group = group_number;
	xkl_engine_allow_one_switch_to_secondary_group (engine);
	cur = xkl_engine_get_current_window (engine);
	if (cur != (Window) NULL) {
		xkl_debug (150, "Enforcing the state %d for window %lx\n",
			   st.group, cur);
		xkl_engine_save_state (engine,
				       xkl_engine_get_current_window
				       (engine), &st);
/*    XSetInputFocus(GDK_DISPLAY_XDISPLAY(gdk_display_get_default()), cur, RevertToNone, CurrentTime );*/
	} else {
		xkl_debug (150,
			   "??? Enforcing the state %d for unknown window\n",
			   st.group);
		/* strange situation - bad things can happen */
	}
	xkl_engine_lock_group (engine, st.group);
}

static void
status_icon_popup_menu_cb (GtkStatusIcon * icon, guint button, guint time)
{
	GtkMenu *popup_menu = GTK_MENU (gtk_menu_new ());
	GtkMenu *groups_menu = GTK_MENU (gtk_menu_new ());
	int i = 0;
	gchar **current_name = matekbd_status_get_group_names ();

	GtkWidget *item = gtk_menu_item_new_with_mnemonic (_("_Layouts"));
	gtk_widget_show (item);
	gtk_menu_shell_append (GTK_MENU_SHELL (popup_menu), item);
	gtk_menu_item_set_submenu (GTK_MENU_ITEM (item),
				   GTK_WIDGET (groups_menu));

	item =
	    gtk_menu_item_new_with_mnemonic (_("Keyboard _Preferences"));
	gtk_widget_show (item);
	g_signal_connect (item, "activate", popup_menu_launch_capplet,
			  NULL);
	gtk_menu_shell_append (GTK_MENU_SHELL (popup_menu), item);

	item = gtk_menu_item_new_with_mnemonic (_("Show _Current Layout"));
	gtk_widget_show (item);
	g_signal_connect (item, "activate", popup_menu_show_layout, NULL);
	gtk_menu_shell_append (GTK_MENU_SHELL (popup_menu), item);

	for (i = 0; *current_name; i++, current_name++) {
		gchar *image_file = matekbd_status_get_image_filename (i);

		if (image_file == NULL) {
			item =
			    gtk_menu_item_new_with_label (*current_name);
		} else {
			GdkPixbuf *pixbuf =
			    gdk_pixbuf_new_from_file_at_size (image_file,
							      24, 24,
							      NULL);
			GtkWidget *img =
			    gtk_image_new_from_pixbuf (pixbuf);
			item =
			    gtk_image_menu_item_new_with_label
			    (*current_name);
			gtk_widget_show (img);
			gtk_image_menu_item_set_image (GTK_IMAGE_MENU_ITEM
						       (item), img);
			gtk_image_menu_item_set_always_show_image
			    (GTK_IMAGE_MENU_ITEM (item), TRUE);
			g_free (image_file);
		}
		gtk_widget_show (item);
		gtk_menu_shell_append (GTK_MENU_SHELL (groups_menu), item);
		g_signal_connect (item, "activate",
				  G_CALLBACK (popup_menu_set_group),
				  GINT_TO_POINTER (i));
	}

	gtk_menu_popup (popup_menu, NULL, NULL,
			gtk_status_icon_position_menu,
			(gpointer) icon, button, time);
}

static void
show_hide_icon ()
{
	if (g_strv_length (current_kbd_config.layouts_variants) > 1) {
		if (icon == NULL) {
			/* FIXME add an option to GSettings to disable this? */
			gboolean disable = FALSE;
			if (disable)
				return;

			xkl_debug (150, "Creating new icon\n");
			icon = matekbd_status_new ();
			g_signal_connect (icon, "popup-menu",
					  G_CALLBACK
					  (status_icon_popup_menu_cb),
					  NULL);

		}
	} else {
		if (icon != NULL) {
			xkl_debug (150, "Destroying icon\n");
			g_object_unref (icon);
			icon = NULL;
		}
	}
}

static gboolean
try_activating_xkb_config_if_new (MatekbdKeyboardConfig *
				  current_sys_kbd_config)
{
	/* Activate - only if different! */
	if (!matekbd_keyboard_config_equals
	    (&current_kbd_config, current_sys_kbd_config)) {
		if (matekbd_keyboard_config_activate (&current_kbd_config)) {
			if (pa_callback != NULL) {
				(*pa_callback) (pa_callback_user_data);
				return TRUE;
			}
		} else {
			return FALSE;
		}
	}
	return TRUE;
}

static gboolean
filter_xkb_config (void)
{
	XklConfigItem *item;
	gchar *lname;
	gchar *vname;
	gchar **lv;
	int i;
	gboolean any_change = FALSE;

	xkl_debug (100, "Filtering configuration against the registry\n");
	if (!xkl_registry) {
		xkl_registry =
		    xkl_config_registry_get_instance (xkl_engine);
		/* load all materials, unconditionally! */
		if (!xkl_config_registry_load (xkl_registry, TRUE)) {
			g_object_unref (xkl_registry);
			xkl_registry = NULL;
			return FALSE;
		}
	}
	lv = g_strdupv(current_kbd_config.layouts_variants);
	item = xkl_config_item_new ();
	for (lv = 0; lv[i] != NULL; i++) {
		xkl_debug (100, "Checking [%s]\n", lv[i]);
		if (matekbd_keyboard_config_split_items
		    (lv[i], &lname, &vname)) {
			g_snprintf (item->name, sizeof (item->name), "%s",
				    lname);
			if (!xkl_config_registry_find_layout
			    (xkl_registry, item)) {
				xkl_debug (100, "Bad layout [%s]\n",
					   lname);
				g_strv_delete_str (current_kbd_config.layouts_variants,
							lv[i]);
				any_change = TRUE;
				continue;
			}
			if (vname) {
				g_snprintf (item->name,
					    sizeof (item->name), "%s",
					    vname);
				if (!xkl_config_registry_find_variant
				    (xkl_registry, lname, item)) {
					xkl_debug (100,
						   "Bad variant [%s(%s)]\n",
						   lname, vname);
					g_strv_delete_str
					    (current_kbd_config.layouts_variants,
					     lv[i]);
					any_change = TRUE;
					continue;
				}
			}
		}
	}
	g_object_unref (item);
	return any_change;
}

static void
apply_xkb_settings (void)
{
	MatekbdKeyboardConfig current_sys_kbd_config;

	if (!inited_ok)
		return;

	matekbd_keyboard_config_init (&current_sys_kbd_config, xkl_engine);

	matekbd_keyboard_config_load_from_gsettings (&current_kbd_config,
					      &initial_sys_kbd_config);

	matekbd_keyboard_config_load_from_x_current (&current_sys_kbd_config,
						  NULL);

	if (!try_activating_xkb_config_if_new (&current_sys_kbd_config)) {
		if (filter_xkb_config ()) {
			if (!try_activating_xkb_config_if_new
			    (&current_sys_kbd_config)) {
				g_warning
				    ("Could not activate the filtered XKB configuration");
				activation_error ();
			}
		} else {
			g_warning
			    ("Could not activate the XKB configuration");
			activation_error ();
		}
	} else
		xkl_debug (100,
			   "Actual KBD configuration was not changed: redundant notification\n");

	matekbd_keyboard_config_term (&current_sys_kbd_config);
	show_hide_icon ();
}

static void
apply_xkb_settings_cb (GSettings *settings, gchar *key, gpointer   user_data)
{
    apply_xkb_settings ();
}

static void
msd_keyboard_xkb_analyze_sysconfig (void)
{
	if (!inited_ok)
		return;

	matekbd_keyboard_config_init (&initial_sys_kbd_config, xkl_engine);
	matekbd_keyboard_config_load_from_x_initial (&initial_sys_kbd_config,
						  NULL);
}

static gboolean
msd_chk_file_list (void)
{
	GDir *home_dir;
	const char *fname;
	GSList *file_list = NULL;
	GSList *last_login_file_list = NULL;
	GSList *tmp = NULL;
	GSList *tmp_l = NULL;
	gboolean new_file_exist = FALSE;

	home_dir = g_dir_open (g_get_home_dir (), 0, NULL);
	while ((fname = g_dir_read_name (home_dir)) != NULL) {
		if (g_strrstr (fname, "modmap")) {
			file_list =
			    g_slist_append (file_list, g_strdup (fname));
		}
	}
	g_dir_close (home_dir);

	gchar **settings_list;
	settings_list = g_settings_get_strv (settings_desktop, KNOWN_FILES_KEY);
	if (settings_list != NULL) {
		gint i;
		for (i = 0; i < G_N_ELEMENTS (settings_list); i++) {
			if (settings_list[i] != NULL)
				last_login_file_list = 
					g_slist_append (last_login_file_list, g_strdup (settings_list[i]));
		}
		g_strfreev (settings_list);
	}

	/* Compare between the two file list, currently available modmap files
	   and the files available in the last log in */
	tmp = file_list;
	while (tmp != NULL) {
		tmp_l = last_login_file_list;
		new_file_exist = TRUE;
		while (tmp_l != NULL) {
			if (strcmp (tmp->data, tmp_l->data) == 0) {
				new_file_exist = FALSE;
				break;
			} else {
				tmp_l = tmp_l->next;
			}
		}
		if (new_file_exist) {
			break;
		} else {
			tmp = tmp->next;
		}
	}

	if (new_file_exist) {
		GSList *l;
		GPtrArray *array = g_ptr_array_new ();
		for (l = file_list; l != NULL; l = l->next)
			g_ptr_array_add (array, l->data);
		g_ptr_array_add (array, NULL);
		g_settings_set_strv (settings_desktop, KNOWN_FILES_KEY, (const gchar **) array->pdata);
		g_ptr_array_free (array, FALSE);
	}

	g_slist_foreach (file_list, (GFunc) g_free, NULL);
	g_slist_free (file_list);

	g_slist_foreach (last_login_file_list, (GFunc) g_free, NULL);
	g_slist_free (last_login_file_list);

	return new_file_exist;

}

static void
msd_keyboard_xkb_chk_lcl_xmm (void)
{
	if (msd_chk_file_list ()) {
		msd_modmap_dialog_call ();
	}
	msd_load_modmap_files ();
}

void
msd_keyboard_xkb_set_post_activation_callback (PostActivationCallback fun,
					       void *user_data)
{
	pa_callback = fun;
	pa_callback_user_data = user_data;
}

static GdkFilterReturn
msd_keyboard_xkb_evt_filter (GdkXEvent * xev, GdkEvent * event)
{
	XEvent *xevent = (XEvent *) xev;
	xkl_engine_filter_events (xkl_engine, xevent);
	return GDK_FILTER_CONTINUE;
}

/* When new Keyboard is plugged in - reload the settings */
static void
msd_keyboard_new_device (XklEngine * engine)
{
	apply_desktop_settings ();
	apply_xkb_settings ();
}

static void
msd_keyboard_update_indicator_icons ()
{
	Bool state;
	int new_state, i;
	Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
	XkbGetNamedIndicator (display, caps_lock, NULL, &state,
			      NULL, NULL);
	new_state = state ? 1 : 0;
	XkbGetNamedIndicator (display, num_lock, NULL, &state, NULL, NULL);
	new_state <<= 1;
	new_state |= (state ? 1 : 0);
	XkbGetNamedIndicator (display, scroll_lock, NULL, &state,
			      NULL, NULL);
	new_state <<= 1;
	new_state |= (state ? 1 : 0);
	xkl_debug (160, "Indicators state: %d\n", new_state);


	for (i = sizeof (indicator_icons) / sizeof (indicator_icons[0]);
	     --i >= 0;) {
		gtk_status_icon_set_from_icon_name (indicator_icons[i],
						    (new_state & (1 << i))
						    ?
						    indicator_on_icon_names
						    [i] :
						    indicator_off_icon_names
						    [i]);
	}
}

static void
msd_keyboard_state_changed (XklEngine * engine, XklEngineStateChange type,
			    gint new_group, gboolean restore)
{
	xkl_debug (160,
		   "State changed: type %d, new group: %d, restore: %d.\n",
		   type, new_group, restore);
	if (type == INDICATORS_CHANGED) {
		msd_keyboard_update_indicator_icons ();
	}
}

void
msd_keyboard_xkb_init (MsdKeyboardManager * kbd_manager)
{
	int i;
	Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
	mate_settings_profile_start (NULL);

	gtk_icon_theme_append_search_path (gtk_icon_theme_get_default (),
					   DATADIR G_DIR_SEPARATOR_S
					   "icons");

	caps_lock = XInternAtom (display, "Caps Lock", False);
	num_lock = XInternAtom (display, "Num Lock", False);
	scroll_lock = XInternAtom (display, "Scroll Lock", False);

	for (i = sizeof (indicator_icons) / sizeof (indicator_icons[0]);
	     --i >= 0;) {
		indicator_icons[i] =
		    gtk_status_icon_new_from_icon_name
		    (indicator_off_icon_names[i]);
	}

	msd_keyboard_update_indicator_icons ();

#ifdef MSDKX
	xkl_set_debug_level (200);
	logfile = fopen ("/tmp/msdkx.log", "a");
	xkl_set_log_appender (msd_keyboard_log_appender);
#endif
	manager = kbd_manager;
	mate_settings_profile_start ("xkl_engine_get_instance");
	xkl_engine = xkl_engine_get_instance (display);
	mate_settings_profile_end ("xkl_engine_get_instance");
	if (xkl_engine) {
		inited_ok = TRUE;

		settings_desktop = g_settings_new (MATEKBD_DESKTOP_SCHEMA);
		settings_kbd = g_settings_new (MATEKBD_KBD_SCHEMA);

		matekbd_desktop_config_init (&current_config,
					     xkl_engine);
		matekbd_keyboard_config_init (&current_kbd_config,
					      xkl_engine);
		xkl_engine_backup_names_prop (xkl_engine);
		msd_keyboard_xkb_analyze_sysconfig ();
		mate_settings_profile_start
		    ("msd_keyboard_xkb_chk_lcl_xmm");
		msd_keyboard_xkb_chk_lcl_xmm ();
		mate_settings_profile_end
		    ("msd_keyboard_xkb_chk_lcl_xmm");

		g_signal_connect (settings_desktop, "changed", G_CALLBACK(apply_desktop_settings_cb), NULL);
		g_signal_connect (settings_kbd, "changed", G_CALLBACK(apply_xkb_settings_cb), NULL);

		gdk_window_add_filter (NULL, (GdkFilterFunc)
				       msd_keyboard_xkb_evt_filter, NULL);

		if (xkl_engine_get_features (xkl_engine) &
		    XKLF_DEVICE_DISCOVERY)
			g_signal_connect (xkl_engine, "X-new-device",
					  G_CALLBACK
					  (msd_keyboard_new_device), NULL);
		g_signal_connect (xkl_engine, "X-state-changed",
				  G_CALLBACK
				  (msd_keyboard_state_changed), NULL);

		mate_settings_profile_start ("xkl_engine_start_listen");
		xkl_engine_start_listen (xkl_engine,
					 XKLL_MANAGE_LAYOUTS |
					 XKLL_MANAGE_WINDOW_STATES);
		mate_settings_profile_end ("xkl_engine_start_listen");

		mate_settings_profile_start ("apply_desktop_settings");
		apply_desktop_settings ();
		mate_settings_profile_end ("apply_desktop_settings");
		mate_settings_profile_start ("apply_xkb_settings");
		apply_xkb_settings ();
		mate_settings_profile_end ("apply_xkb_settings");
	}
	preview_dialogs = g_hash_table_new (g_direct_hash, g_direct_equal);

	mate_settings_profile_end (NULL);
}

void
msd_keyboard_xkb_shutdown (void)
{
	int i;

	pa_callback = NULL;
	pa_callback_user_data = NULL;
	manager = NULL;

	for (i = sizeof (indicator_icons) / sizeof (indicator_icons[0]);
	     --i >= 0;) {
		g_object_unref (G_OBJECT (indicator_icons[i]));
		indicator_icons[i] = NULL;
	}

	g_hash_table_destroy (preview_dialogs);

	if (!inited_ok)
		return;

	xkl_engine_stop_listen (xkl_engine,
				XKLL_MANAGE_LAYOUTS |
				XKLL_MANAGE_WINDOW_STATES);

	gdk_window_remove_filter (NULL, (GdkFilterFunc)
				  msd_keyboard_xkb_evt_filter, NULL);

	if (settings_desktop != NULL) {
		g_object_unref (settings_desktop);
	}

	if (settings_kbd != NULL) {
		g_object_unref (settings_kbd);
	}

	if (xkl_registry) {
		g_object_unref (xkl_registry);
	}

	g_object_unref (xkl_engine);

	xkl_engine = NULL;
	inited_ok = FALSE;
}
