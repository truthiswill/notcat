/* Copyright 2018 Jack Conger */

/*
 * This file is part of notcat.
 *
 * notcat is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * notcat is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with notcat.  If not, see <http://www.gnu.org/licenses/>.
 *
 * GDBus logic has been liberally adapted from
 * https://github.com/dunst-project/dunst/blob/master/src/dbus.c
 *
 * Desktop notifications API details can be found at
 * https://developer.gnome.org/notification-spec/
 */

#include <glib.h>
#include <gio/gio.h>
#include <stdio.h>

#include "notcat.h"

/*
 * dbussy data type pseudo-boilerplate.  The introspection_xml parsing might
 * actually be the least unreadable way to express this information,
 * horrifyingly.
 */

static GDBusNodeInfo *introspection_data = NULL;

static const char *introspection_xml =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<node name=\""FDN_PATH"\">"
    "    <interface name=\""FDN_IFAC"\">"

    "        <method name=\"GetCapabilities\">"
    "            <arg direction=\"out\" name=\"capabilities\"    type=\"as\"/>"
    "        </method>"

    "        <method name=\"Notify\">"
    "            <arg direction=\"in\"  name=\"app_name\"        type=\"s\"/>"
    "            <arg direction=\"in\"  name=\"replaces_id\"     type=\"u\"/>"
    "            <arg direction=\"in\"  name=\"app_icon\"        type=\"s\"/>"
    "            <arg direction=\"in\"  name=\"summary\"         type=\"s\"/>"
    "            <arg direction=\"in\"  name=\"body\"            type=\"s\"/>"
    "            <arg direction=\"in\"  name=\"actions\"         type=\"as\"/>"
    "            <arg direction=\"in\"  name=\"hints\"           type=\"a{sv}\"/>"
    "            <arg direction=\"in\"  name=\"expire_timeout\"  type=\"i\"/>"
    "            <arg direction=\"out\" name=\"id\"              type=\"u\"/>"
    "        </method>"

    "        <method name=\"CloseNotification\">"
    "            <arg direction=\"in\"  name=\"id\"              type=\"u\"/>"
    "        </method>"

    "        <method name=\"GetServerInformation\">"
    "            <arg direction=\"out\" name=\"name\"            type=\"s\"/>"
    "            <arg direction=\"out\" name=\"vendor\"          type=\"s\"/>"
    "            <arg direction=\"out\" name=\"version\"         type=\"s\"/>"
    "            <arg direction=\"out\" name=\"spec_version\"    type=\"s\"/>"
    "        </method>"

    "        <signal name=\"NotificationClosed\">"
    "            <arg name=\"id\"         type=\"u\"/>"
    "            <arg name=\"reason\"     type=\"u\"/>"
    "        </signal>"

    "        <signal name=\"ActionInvoked\">"
    "            <arg name=\"id\"         type=\"u\"/>"
    "            <arg name=\"action_key\" type=\"s\"/>"
    "        </signal>"
    "   </interface>"
"</node>";

/*
 * Per-method-call logic.  Obviously, the most interesting is notify().
 */

static void get_capabilities(GDBusConnection *conn, const gchar *sender,
                             const GVariant *params,
                             GDBusMethodInvocation *invocation) {
    GVariantBuilder *builder;
    GVariant *value;

    builder = g_variant_builder_new(G_VARIANT_TYPE("as"));
#if ACTIONS
    g_variant_builder_add(builder, "s", "actions");
#endif
    g_variant_builder_add(builder, "s", "body");
    /*
     * g_variant_builder_add(builder, "s", "body-hyperlinks");
     * g_variant_builder_add(builder, "s", "body-markup");
     */

    value = g_variant_new("(as)", builder);
    g_variant_builder_unref(builder);
    g_dbus_method_invocation_return_value(invocation, value);

    g_dbus_connection_flush(conn, NULL, NULL, NULL);
}

static unsigned int id = 0;

static void notify(GDBusConnection *conn, const gchar *sender,
                   GVariant *params,
                   GDBusMethodInvocation *invocation) {
    char *appname = NULL;
    unsigned int replaces_id = 0;
    char *summary = NULL;
    char *body = NULL;
#if ACTIONS
    Actions *actions = g_malloc0(sizeof(Actions));
#endif
    int timeout = -1;
    int urgency = 1;

    {
        GVariantIter *iter = g_variant_iter_new(params);
        GVariant *content;
        GVariant *dict_value;
        int idx = 0;
        while ((content = g_variant_iter_next_value(iter))) {
            switch (idx) {
            case 0:
                if (g_variant_is_of_type(content, G_VARIANT_TYPE_STRING))
                    appname = g_variant_dup_string(content, NULL);
                break;
            case 1:
                if (g_variant_is_of_type(content, G_VARIANT_TYPE_UINT32))
                    replaces_id = g_variant_get_uint32(content);
                break;
            case 2: break;  /* icon -- not supported */
            case 3:
                if (g_variant_is_of_type(content, G_VARIANT_TYPE_STRING))
                    summary = g_variant_dup_string(content, NULL);
                break;
            case 4:
                if (g_variant_is_of_type(content, G_VARIANT_TYPE_STRING))
                    body = g_variant_dup_string(content, NULL);
                break;
            case 5:
#if ACTIONS
                if (g_variant_is_of_type(content, G_VARIANT_TYPE_STRING_ARRAY))
                    actions->actions = g_variant_dup_strv(content, &(actions->count));
#endif
                break;
            case 6:
                if (g_variant_is_of_type(content, G_VARIANT_TYPE_DICTIONARY)) {
                    dict_value = g_variant_lookup_value(content, "urgency",
                                                        G_VARIANT_TYPE_BYTE);
                    if (dict_value) {
                        urgency = g_variant_get_byte(dict_value);
                        g_variant_unref(dict_value);
                    }
                    // TODO: custom formats
                    // TODO: category (?)
                    // TODO: support tying arbitrary hints to notifications
                }
                break;
            case 7:
                if (g_variant_is_of_type(content, G_VARIANT_TYPE_INT32))
                    timeout = g_variant_get_int32(content);
                break;
            }
            g_variant_unref(content);
            idx++;
        }

        g_variant_iter_free(iter);
    }

    unsigned int n_id = replaces_id ? replaces_id : ++id;

#if ACTIONS
    if (actions->count < 1) {
        free_actions(actions);
        actions = NULL;
    }
#endif

    /* At some point this will be more sophisticated, lol */
    Note *note = new_note(n_id, appname, summary, body,
#if ACTIONS
                          actions,
#endif
                          timeout, urgency);
    print_note(note);
    free_note(note);

    GVariant *reply = g_variant_new("(u)", n_id);
    g_dbus_method_invocation_return_value(invocation, reply);
    g_dbus_connection_flush(conn, NULL, NULL, NULL);
}

static void close_notification(GDBusConnection *conn, const gchar *sender,
                               GVariant *params,
                               GDBusMethodInvocation *invocation) {
    guint32 id;
    g_variant_get(params, "(u)", &id);

    // TODO: Do the thing that actually "closes" the notification, aha

    g_dbus_method_invocation_return_value(invocation, NULL);
    g_dbus_connection_flush(conn, NULL, NULL, NULL);
}

static void get_server_information(GDBusConnection *conn, const gchar *sender,
                                   const GVariant *params,
                                   GDBusMethodInvocation *invocation) {
    GVariant *value;
    value = g_variant_new("(ssss)", "notcat", "jpco", VERSION, "1.2");
    g_dbus_method_invocation_return_value(invocation, value);
    g_dbus_connection_flush(conn, NULL, NULL, NULL);
}

/*
 * Signals.  We may send these for Reasons.
 */

/*
void signal_notification_closed(GDBusConnection *conn,
                                const guint not_id, const guint reason) {
    GVariant *body;
    GError *err;

    body = g_variant_new("(uu)", not_id, reason);
    err = NULL;

    g_dbus_connection_emit_signal(conn, dbus_client,
                                  FDN_PATH, FDN_IFAC, "NotificationClosed",
                                  body, &err);
    if (err) {
        g_printerr("Could not close notification: %s\n", err->message);
        g_error_free(err);
    }
}
*/

/*
 * Scaffolding functions.  These put things together properly.
 */

void handle_method_call(GDBusConnection *conn,
                        const gchar *sender,
                        const gchar *object_path,
                        const gchar *interface_name,
                        const gchar *method_name,
                        GVariant *params,
                        GDBusMethodInvocation *invocation,
                        gpointer user_data) {
    if (g_strcmp0(method_name, "GetCapabilities") == 0) {
        get_capabilities(conn, sender, params, invocation);
    } else if (g_strcmp0(method_name, "Notify") == 0) {
        notify(conn, sender, params, invocation);
    } else if (g_strcmp0(method_name, "CloseNotification") == 0) {
        close_notification(conn, sender, params, invocation);
    } else if (g_strcmp0(method_name, "GetServerInformation") == 0) {
        get_server_information(conn, sender, params, invocation);
    } else {
        g_printerr("Unknown method name '%s' received from '%s'\n",
                   method_name, sender);
    }
}

static const GDBusInterfaceVTable interface_vtable = {
    handle_method_call
};

static void on_bus_acquired(GDBusConnection *conn, const gchar *name,
                            gpointer user_data) {
    guint reg_id;
    GError *err = NULL;

    reg_id = g_dbus_connection_register_object(conn, FDN_PATH,
                                               introspection_data->interfaces[0],
                                               &interface_vtable,
                                               NULL, NULL, &err);

    if (reg_id == 0) {
        g_printerr("Urgh, failed to register object: %s\n", err->message);
    }
}

static void on_name_acquired(GDBusConnection *conn, const gchar *name,
                             gpointer user_data) {
    // g_printerr("Got name %s on the session bus\n", name);
}

static void on_name_lost(GDBusConnection *conn, const gchar *name,
                         gpointer user_data) {
    // g_printerr("Lost name %s on the session bus\n", name);
}

extern void run(void) {
    GMainLoop *loop;
    guint owner_id;

    introspection_data = g_dbus_node_info_new_for_xml(introspection_xml,
                                                      NULL);

    owner_id = g_bus_own_name(G_BUS_TYPE_SESSION,
            FDN_NAME,
            G_BUS_NAME_OWNER_FLAGS_NONE,
            on_bus_acquired,
            on_name_acquired,
            on_name_lost,
            NULL,
            NULL);

    loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(loop);

    g_bus_unown_name(owner_id);
}