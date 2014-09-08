/* -*- Mode: C; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 8 -*-
 *
 * Copyright (C) 2007 William Jon McCann <mccann@jhu.edu>
 * Copyright (C) 2010,2011 Red Hat, Inc.
 *
 * Author: Bastien Nocera <hadess@hadess.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 */

#include "config.h"

#include <gio/gio.h>
#include <string.h>

#include "gnome-settings-plugin.h"
#include "gnome-settings-profile.h"
#include "gsd-rfkill-manager.h"
#include "rfkill-glib.h"

#define GSD_RFKILL_MANAGER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), GSD_TYPE_RFKILL_MANAGER, GsdRfkillManagerPrivate))

struct GsdRfkillManagerPrivate
{
        GDBusNodeInfo           *introspection_data;
        guint                    name_id;
        GDBusConnection         *connection;

        CcRfkillGlib            *rfkill;
        GHashTable              *killswitches;

        GCancellable            *cancellable;

        GDBusProxy              *hostnamed_client;
        gchar                   *chassis_type;
};

#define GSD_RFKILL_DBUS_NAME GSD_DBUS_NAME ".Rfkill"
#define GSD_RFKILL_DBUS_PATH GSD_DBUS_PATH "/Rfkill"

static const gchar introspection_xml[] =
"<node>"
"  <interface name='org.gnome.SettingsDaemon.Rfkill'>"
"    <annotation name='org.freedesktop.DBus.GLib.CSymbol' value='gsd_rfkill_manager'/>"
"      <property name='AirplaneMode' type='b' access='readwrite'/>"
"      <property name='HasAirplaneMode' type='b' access='read'/>"
"      <property name='ShouldShowAirplaneMode' type='b' access='read'/>"
"  </interface>"
"</node>";

static void     gsd_rfkill_manager_class_init  (GsdRfkillManagerClass *klass);
static void     gsd_rfkill_manager_init        (GsdRfkillManager      *rfkill_manager);
static void     gsd_rfkill_manager_finalize    (GObject                    *object);

G_DEFINE_TYPE (GsdRfkillManager, gsd_rfkill_manager, G_TYPE_OBJECT)

static gpointer manager_object = NULL;

static void
gsd_rfkill_manager_class_init (GsdRfkillManagerClass *klass)
{
        GObjectClass   *object_class = G_OBJECT_CLASS (klass);

        object_class->finalize = gsd_rfkill_manager_finalize;

        g_type_class_add_private (klass, sizeof (GsdRfkillManagerPrivate));
}

static void
gsd_rfkill_manager_init (GsdRfkillManager *manager)
{
        manager->priv = GSD_RFKILL_MANAGER_GET_PRIVATE (manager);
}

static gboolean
engine_get_airplane_mode (GsdRfkillManager *manager)
{
	GHashTableIter iter;
	gpointer key, value;

        /* If we have no killswitches, airplane mode is off. */
        if (g_hash_table_size (manager->priv->killswitches) == 0) {
                return FALSE;
        }

	g_hash_table_iter_init (&iter, manager->priv->killswitches);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		gboolean state;

		state = GPOINTER_TO_INT (value);

		/* A single rfkill switch that's disabled? Airplane mode is off */
		if (!state) {
                        return FALSE;
                }
	}

        return TRUE;
}

static gboolean
engine_get_has_airplane_mode (GsdRfkillManager *manager)
{
        return (g_hash_table_size (manager->priv->killswitches) > 0);
}

static gboolean
engine_get_should_show_airplane_mode (GsdRfkillManager *manager)
{
        return (g_strcmp0 (manager->priv->chassis_type, "desktop") != 0) &&
                (g_strcmp0 (manager->priv->chassis_type, "server") != 0) &&
                (g_strcmp0 (manager->priv->chassis_type, "vm") != 0) &&
                (g_strcmp0 (manager->priv->chassis_type, "container") != 0);
}

static void
engine_properties_changed (GsdRfkillManager *manager)
{
        GVariantBuilder props_builder;
        GVariant *props_changed = NULL;

        g_variant_builder_init (&props_builder, G_VARIANT_TYPE ("a{sv}"));

        g_variant_builder_add (&props_builder, "{sv}", "AirplaneMode",
                               g_variant_new_boolean (engine_get_airplane_mode (manager)));
        g_variant_builder_add (&props_builder, "{sv}", "HasAirplaneMode",
                               g_variant_new_boolean (engine_get_has_airplane_mode (manager)));
        g_variant_builder_add (&props_builder, "{sv}", "ShouldShowAirplaneMode",
                               g_variant_new_boolean (engine_get_should_show_airplane_mode (manager)));

        props_changed = g_variant_new ("(s@a{sv}@as)", GSD_RFKILL_DBUS_NAME,
                                       g_variant_builder_end (&props_builder),
                                       g_variant_new_strv (NULL, 0));

        g_dbus_connection_emit_signal (manager->priv->connection,
                                       NULL,
                                       GSD_RFKILL_DBUS_PATH,
                                       "org.freedesktop.DBus.Properties",
                                       "PropertiesChanged",
                                       props_changed, NULL);
}

static void
rfkill_changed (CcRfkillGlib     *rfkill,
		GList            *events,
		GsdRfkillManager  *manager)
{
	GList *l;

	for (l = events; l != NULL; l = l->next) {
		struct rfkill_event *event = l->data;

                switch (event->op) {
                case RFKILL_OP_ADD:
                case RFKILL_OP_CHANGE:
			g_hash_table_insert (manager->priv->killswitches,
					     GINT_TO_POINTER (event->idx),
					     GINT_TO_POINTER (event->soft || event->hard));
                        break;
                case RFKILL_OP_DEL:
			g_hash_table_remove (manager->priv->killswitches,
					     GINT_TO_POINTER (event->idx));
                        break;
                }
	}

        /* not yet connected to the bus */
        if (manager->priv->connection == NULL)
                return;

        engine_properties_changed (manager);
}

static void
rfkill_set_cb (GObject      *source_object,
	       GAsyncResult *res,
	       gpointer      user_data)
{
	gboolean ret;
	GError *error = NULL;

	ret = cc_rfkill_glib_send_event_finish (CC_RFKILL_GLIB (source_object), res, &error);
	if (!ret) {
		g_warning ("Failed to set RFKill: %s", error->message);
		g_error_free (error);
	}
}

static gboolean
engine_set_airplane_mode (GsdRfkillManager *manager,
                          gboolean          enable)
{
	struct rfkill_event event;

	memset (&event, 0, sizeof(event));
	event.op = RFKILL_OP_CHANGE_ALL;
	event.type = RFKILL_TYPE_ALL;
	event.soft = enable ? 1 : 0;
	cc_rfkill_glib_send_event (manager->priv->rfkill, &event, NULL, rfkill_set_cb, manager);

	return TRUE;
}

static gboolean
handle_set_property (GDBusConnection *connection,
                     const gchar     *sender,
                     const gchar     *object_path,
                     const gchar     *interface_name,
                     const gchar     *property_name,
                     GVariant        *value,
                     GError         **error,
                     gpointer         user_data)
{
        GsdRfkillManager *manager = GSD_RFKILL_MANAGER (user_data);

        if (g_strcmp0 (property_name, "AirplaneMode") == 0) {
                gboolean airplane_mode;
                g_variant_get (value, "b", &airplane_mode);
                return engine_set_airplane_mode (manager, airplane_mode);
        }

        return FALSE;
}

static GVariant *
handle_get_property (GDBusConnection *connection,
                     const gchar     *sender,
                     const gchar     *object_path,
                     const gchar     *interface_name,
                     const gchar     *property_name,
                     GError         **error,
                     gpointer         user_data)
{
        GsdRfkillManager *manager = GSD_RFKILL_MANAGER (user_data);

        /* Check session pointer as a proxy for whether the manager is in the
           start or stop state */
        if (manager->priv->connection == NULL) {
                return NULL;
        }

        if (g_strcmp0 (property_name, "AirplaneMode") == 0) {
                gboolean airplane_mode;
                airplane_mode = engine_get_airplane_mode (manager);
                return g_variant_new_boolean (airplane_mode);
        }

        if (g_strcmp0 (property_name, "ShouldShowAirplaneMode") == 0) {
                gboolean should_show_airplane_mode;
                should_show_airplane_mode = engine_get_should_show_airplane_mode (manager);
                return g_variant_new_boolean (should_show_airplane_mode);
        }

        if (g_strcmp0 (property_name, "HasAirplaneMode") == 0) {
                gboolean has_airplane_mode;
                has_airplane_mode = engine_get_has_airplane_mode (manager);
                return g_variant_new_boolean (has_airplane_mode);
        }

        return NULL;
}

static const GDBusInterfaceVTable interface_vtable =
{
        NULL,
        handle_get_property,
        handle_set_property
};

static void
on_bus_gotten (GObject               *source_object,
               GAsyncResult          *res,
               GsdRfkillManager *manager)
{
        GDBusConnection *connection;
        GError *error = NULL;

        connection = g_bus_get_finish (res, &error);
        if (connection == NULL) {
                g_warning ("Could not get session bus: %s", error->message);
                g_error_free (error);
                return;
        }
        manager->priv->connection = connection;

        g_dbus_connection_register_object (connection,
                                           GSD_RFKILL_DBUS_PATH,
                                           manager->priv->introspection_data->interfaces[0],
                                           &interface_vtable,
                                           manager,
                                           NULL,
                                           NULL);

        manager->priv->name_id = g_bus_own_name_on_connection (connection,
                                                               GSD_RFKILL_DBUS_NAME,
                                                               G_BUS_NAME_OWNER_FLAGS_NONE,
                                                               NULL,
                                                               NULL,
                                                               NULL,
                                                               NULL);
}

static void
sync_chassis_type (GsdRfkillManager *manager)
{
        GVariant *property;
        const gchar *chassis_type;

        property = g_dbus_proxy_get_cached_property (manager->priv->hostnamed_client,
                                                     "Chassis");

        if (property == NULL)
                return;

        chassis_type = g_variant_get_string (property, NULL);
        if (g_strcmp0 (manager->priv->chassis_type, chassis_type) != 0) {
                g_free (manager->priv->chassis_type);
                manager->priv->chassis_type = g_strdup (chassis_type);

                engine_properties_changed (manager);
        }

        g_variant_unref (property);
}

static void
hostnamed_properties_changed (GDBusProxy *proxy,
                              GVariant   *changed_properties,
                              GStrv       invalidated_properties,
                              gpointer    user_data)
{
        GsdRfkillManager *manager = user_data;
        GVariant *value;

        value = g_variant_lookup_value (changed_properties, "Chassis", G_VARIANT_TYPE_STRING);
        if (value != NULL) {
                sync_chassis_type (manager);
                g_variant_unref (value);
        }
}

static void
on_hostnamed_proxy_gotten (GObject      *source,
                           GAsyncResult *result,
                           gpointer      user_data)
{
        GsdRfkillManager *manager = user_data;
        GDBusProxy *proxy;
        GError *error;

        error = NULL;
        proxy = g_dbus_proxy_new_for_bus_finish (result, &error);

        if (proxy == NULL) {
                if (!g_error_matches (error, G_IO_ERROR, G_IO_ERROR_CANCELLED) &&
                    !g_error_matches (error, G_DBUS_ERROR, G_DBUS_ERROR_SERVICE_UNKNOWN))
                        g_warning ("Failed to acquire hostnamed proxy: %s", error->message);

                g_error_free (error);
                goto out;
        }

        manager->priv->hostnamed_client = proxy;

        g_signal_connect (manager->priv->hostnamed_client, "g-properties-changed",
                          G_CALLBACK (hostnamed_properties_changed), manager);
        sync_chassis_type (manager);

 out:
        g_object_unref (manager);
}

gboolean
gsd_rfkill_manager_start (GsdRfkillManager *manager,
                         GError         **error)
{
        gnome_settings_profile_start (NULL);

        manager->priv->introspection_data = g_dbus_node_info_new_for_xml (introspection_xml, NULL);
        g_assert (manager->priv->introspection_data != NULL);

        manager->priv->killswitches = g_hash_table_new (g_direct_hash, g_direct_equal);
        manager->priv->rfkill = cc_rfkill_glib_new ();
        g_signal_connect (G_OBJECT (manager->priv->rfkill), "changed",
                          G_CALLBACK (rfkill_changed), manager);
        cc_rfkill_glib_open (manager->priv->rfkill);

        manager->priv->cancellable = g_cancellable_new ();

        g_dbus_proxy_new_for_bus (G_BUS_TYPE_SYSTEM,
                                  G_DBUS_PROXY_FLAGS_NONE,
                                  NULL, /* g-interface-info */
                                  "org.freedesktop.hostname1",
                                  "/org/freedesktop/hostname1",
                                  "org.freedesktop.hostname1",
                                  manager->priv->cancellable,
                                  on_hostnamed_proxy_gotten, g_object_ref (manager));

        /* Start process of owning a D-Bus name */
        g_bus_get (G_BUS_TYPE_SESSION,
                   NULL,
                   (GAsyncReadyCallback) on_bus_gotten,
                   manager);

        gnome_settings_profile_end (NULL);

        return TRUE;
}

void
gsd_rfkill_manager_stop (GsdRfkillManager *manager)
{
        GsdRfkillManagerPrivate *p = manager->priv;

        g_debug ("Stopping rfkill manager");

        if (manager->priv->name_id != 0)
                g_bus_unown_name (manager->priv->name_id);

        g_clear_pointer (&p->introspection_data, g_dbus_node_info_unref);
        g_clear_object (&p->connection);
        g_clear_object (&p->rfkill);
        g_clear_pointer (&p->killswitches, g_hash_table_destroy);

        if (p->cancellable) {
                g_cancellable_cancel (p->cancellable);
                g_clear_object (&p->cancellable);
        }

        g_clear_object (&p->hostnamed_client);
        g_clear_pointer (&p->chassis_type, g_free);
}

static void
gsd_rfkill_manager_finalize (GObject *object)
{
        GsdRfkillManager *manager;

        g_return_if_fail (object != NULL);
        g_return_if_fail (GSD_IS_RFKILL_MANAGER (object));

        manager = GSD_RFKILL_MANAGER (object);

        g_return_if_fail (manager->priv != NULL);

        gsd_rfkill_manager_stop (manager);

        G_OBJECT_CLASS (gsd_rfkill_manager_parent_class)->finalize (object);
}

GsdRfkillManager *
gsd_rfkill_manager_new (void)
{
        if (manager_object != NULL) {
                g_object_ref (manager_object);
        } else {
                manager_object = g_object_new (GSD_TYPE_RFKILL_MANAGER, NULL);
                g_object_add_weak_pointer (manager_object,
                                           (gpointer *) &manager_object);
        }

        return GSD_RFKILL_MANAGER (manager_object);
}
