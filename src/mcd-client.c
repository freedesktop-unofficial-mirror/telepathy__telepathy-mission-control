/* vi: set et sw=4 ts=8 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 8 -*- */
/*
 * Mission Control client proxy.
 *
 * Copyright (C) 2009 Nokia Corporation
 * Copyright (C) 2009 Collabora Ltd.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 */

#include "config.h"
#include "mcd-client-priv.h"

#include <errno.h>

#include <telepathy-glib/dbus.h>
#include <telepathy-glib/defs.h>
#include <telepathy-glib/errors.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/proxy-subclass.h>
#include <telepathy-glib/util.h>

#include "mcd-debug.h"

G_DEFINE_TYPE (McdClientProxy, _mcd_client_proxy, TP_TYPE_CLIENT);

enum
{
    PROP_0,
    PROP_ACTIVATABLE,
    PROP_STRING_POOL,
    PROP_UNIQUE_NAME,
};

enum
{
    S_READY,
    N_SIGNALS
};

static guint signals[N_SIGNALS] = { 0 };

struct _McdClientProxyPrivate
{
    TpHandleRepoIface *string_pool;
    /* Handler.Capabilities, represented as handles taken from
     * dispatcher->priv->string_pool */
    TpHandleSet *capability_tokens;

    gchar *unique_name;
    gboolean ready;
    gboolean bypass_approval;

    /* If a client was in the ListActivatableNames list, it must not be
     * removed when it disappear from the bus.
     */
    gboolean activatable;

    /* Channel filters
     * A channel filter is a GHashTable of
     * - key: gchar *property_name
     * - value: GValue of one of the allowed types on the ObserverChannelFilter
     *          spec. The following matching is observed:
     *           * G_TYPE_STRING: 's'
     *           * G_TYPE_BOOLEAN: 'b'
     *           * DBUS_TYPE_G_OBJECT_PATH: 'o'
     *           * G_TYPE_UINT64: 'y' (8b), 'q' (16b), 'u' (32b), 't' (64b)
     *           * G_TYPE_INT64:            'n' (16b), 'i' (32b), 'x' (64b)
     *
     * The list can be NULL if there is no filter, or the filters are not yet
     * retrieven from the D-Bus *ChannelFitler properties. In the last case,
     * the dispatcher just don't dispatch to this client.
     */
    GList *approver_filters;
    GList *handler_filters;
    GList *observer_filters;
};

static void _mcd_client_proxy_take_approver_filters
    (McdClientProxy *self, GList *filters);
static void _mcd_client_proxy_take_observer_filters
    (McdClientProxy *self, GList *filters);
static void _mcd_client_proxy_take_handler_filters
    (McdClientProxy *self, GList *filters);

static gchar *
_mcd_client_proxy_find_client_file (const gchar *client_name)
{
    const gchar * const *dirs;
    const gchar *dirname;
    const gchar *env_dirname;
    gchar *filename, *absolute_filepath;

    /*
     * The full path is $XDG_DATA_DIRS/telepathy/clients/clientname.client
     * or $XDG_DATA_HOME/telepathy/clients/clientname.client
     * For testing purposes, we also look for $MC_CLIENTS_DIR/clientname.client
     * if $MC_CLIENTS_DIR is set.
     */
    filename = g_strdup_printf ("%s.client", client_name);
    env_dirname = g_getenv ("MC_CLIENTS_DIR");
    if (env_dirname)
    {
        absolute_filepath = g_build_filename (env_dirname, filename, NULL);
        if (g_file_test (absolute_filepath, G_FILE_TEST_IS_REGULAR))
            goto finish;
        g_free (absolute_filepath);
    }

    dirname = g_get_user_data_dir ();
    if (G_LIKELY (dirname))
    {
        absolute_filepath = g_build_filename (dirname, "telepathy/clients",
                                              filename, NULL);
        if (g_file_test (absolute_filepath, G_FILE_TEST_IS_REGULAR))
            goto finish;
        g_free (absolute_filepath);
    }

    dirs = g_get_system_data_dirs ();
    for (dirname = *dirs; dirname != NULL; dirs++, dirname = *dirs)
    {
        absolute_filepath = g_build_filename (dirname, "telepathy/clients",
                                              filename, NULL);
        if (g_file_test (absolute_filepath, G_FILE_TEST_IS_REGULAR))
            goto finish;
        g_free (absolute_filepath);
    }

    absolute_filepath = NULL;
finish:
    g_free (filename);
    return absolute_filepath;
}

static GHashTable *
parse_client_filter (GKeyFile *file, const gchar *group)
{
    GHashTable *filter;
    gchar **keys;
    gsize len = 0;
    guint i;

    filter = g_hash_table_new_full (g_str_hash, g_str_equal, g_free,
                                    (GDestroyNotify) tp_g_value_slice_free);

    keys = g_key_file_get_keys (file, group, &len, NULL);
    for (i = 0; i < len; i++)
    {
        const gchar *key;
        const gchar *space;
        gchar *file_property;
        gchar file_property_type;

        key = keys[i];
        space = g_strrstr (key, " ");

        if (space == NULL || space[1] == '\0' || space[2] != '\0')
        {
            g_warning ("Invalid key %s in client file", key);
            continue;
        }
        file_property_type = space[1];
        file_property = g_strndup (key, space - key);

        switch (file_property_type)
        {
        case 'q':
        case 'u':
        case 't': /* unsigned integer */
            {
                /* g_key_file_get_integer cannot be used because we need
                 * to support 64 bits */
                guint x;
                GValue *value = tp_g_value_slice_new (G_TYPE_UINT64);
                gchar *str = g_key_file_get_string (file, group, key,
                                                    NULL);
                errno = 0;
                x = g_ascii_strtoull (str, NULL, 0);
                if (errno != 0)
                {
                    g_warning ("Invalid unsigned integer '%s' in client"
                               " file", str);
                }
                else
                {
                    g_value_set_uint64 (value, x);
                    g_hash_table_insert (filter, file_property, value);
                }
                g_free (str);
                break;
            }

        case 'y':
        case 'n':
        case 'i':
        case 'x': /* signed integer */
            {
                gint x;
                GValue *value = tp_g_value_slice_new (G_TYPE_INT64);
                gchar *str = g_key_file_get_string (file, group, key, NULL);
                errno = 0;
                x = g_ascii_strtoll (str, NULL, 0);
                if (errno != 0)
                {
                    g_warning ("Invalid signed integer '%s' in client"
                               " file", str);
                }
                else
                {
                    g_value_set_int64 (value, x);
                    g_hash_table_insert (filter, file_property, value);
                }
                g_free (str);
                break;
            }

        case 'b':
            {
                GValue *value = tp_g_value_slice_new (G_TYPE_BOOLEAN);
                gboolean b = g_key_file_get_boolean (file, group, key, NULL);
                g_value_set_boolean (value, b);
                g_hash_table_insert (filter, file_property, value);
                break;
            }

        case 's':
            {
                GValue *value = tp_g_value_slice_new (G_TYPE_STRING);
                gchar *str = g_key_file_get_string (file, group, key, NULL);

                g_value_take_string (value, str);
                g_hash_table_insert (filter, file_property, value);
                break;
            }

        case 'o':
            {
                GValue *value = tp_g_value_slice_new
                    (DBUS_TYPE_G_OBJECT_PATH);
                gchar *str = g_key_file_get_string (file, group, key, NULL);

                g_value_take_boxed (value, str);
                g_hash_table_insert (filter, file_property, value);
                break;
            }

        default:
            g_warning ("Invalid key %s in client file", key);
            continue;
        }
    }
    g_strfreev (keys);

    return filter;
}

static void
parse_client_file (McdClientProxy *client,
                   GKeyFile *file)
{
    gchar **iface_names, **groups, **cap_tokens;
    guint i;
    gsize len = 0;
    gboolean is_approver, is_handler, is_observer;
    GList *approver_filters = NULL;
    GList *observer_filters = NULL;
    GList *handler_filters = NULL;
    gboolean bypass;

    iface_names = g_key_file_get_string_list (file, TP_IFACE_CLIENT,
                                              "Interfaces", 0, NULL);
    if (!iface_names)
        return;

    _mcd_client_proxy_add_interfaces (client,
                                      (const gchar * const *) iface_names);
    g_strfreev (iface_names);

    is_approver = tp_proxy_has_interface_by_id (client,
                                                TP_IFACE_QUARK_CLIENT_APPROVER);
    is_observer = tp_proxy_has_interface_by_id (client,
                                                TP_IFACE_QUARK_CLIENT_OBSERVER);
    is_handler = tp_proxy_has_interface_by_id (client,
                                               TP_IFACE_QUARK_CLIENT_HANDLER);

    /* parse filtering rules */
    groups = g_key_file_get_groups (file, &len);
    for (i = 0; i < len; i++)
    {
        if (is_approver &&
            g_str_has_prefix (groups[i], TP_IFACE_CLIENT_APPROVER
                              ".ApproverChannelFilter "))
        {
            approver_filters =
                g_list_prepend (approver_filters,
                                parse_client_filter (file, groups[i]));
        }
        else if (is_handler &&
            g_str_has_prefix (groups[i], TP_IFACE_CLIENT_HANDLER
                              ".HandlerChannelFilter "))
        {
            handler_filters =
                g_list_prepend (handler_filters,
                                parse_client_filter (file, groups[i]));
        }
        else if (is_observer &&
            g_str_has_prefix (groups[i], TP_IFACE_CLIENT_OBSERVER
                              ".ObserverChannelFilter "))
        {
            observer_filters =
                g_list_prepend (observer_filters,
                                parse_client_filter (file, groups[i]));
        }
    }
    g_strfreev (groups);

    _mcd_client_proxy_take_approver_filters (client,
                                             approver_filters);
    _mcd_client_proxy_take_observer_filters (client,
                                             observer_filters);
    _mcd_client_proxy_take_handler_filters (client,
                                            handler_filters);

    /* Other client options */
    bypass = g_key_file_get_boolean (file, TP_IFACE_CLIENT_HANDLER,
                                     "BypassApproval", NULL);
    _mcd_client_proxy_set_bypass_approval (client, bypass);

    cap_tokens = g_key_file_get_keys (file,
                                      TP_IFACE_CLIENT_HANDLER ".Capabilities",
                                      NULL,
                                      NULL);
    _mcd_client_proxy_add_cap_tokens (client,
                                      (const gchar * const *) cap_tokens);
    g_strfreev (cap_tokens);
}

void
_mcd_client_proxy_set_filters (McdClientProxy *client,
                               McdClientInterface interface,
                               GPtrArray *filters)
{
    GList *client_filters = NULL;
    guint i;

    for (i = 0 ; i < filters->len ; i++)
    {
        GHashTable *channel_class = g_ptr_array_index (filters, i);
        GHashTable *new_channel_class;
        GHashTableIter iter;
        gchar *property_name;
        GValue *property_value;
        gboolean valid_filter = TRUE;

        new_channel_class = g_hash_table_new_full
            (g_str_hash, g_str_equal, g_free,
             (GDestroyNotify) tp_g_value_slice_free);

        g_hash_table_iter_init (&iter, channel_class);
        while (g_hash_table_iter_next (&iter, (gpointer *) &property_name,
                                       (gpointer *) &property_value)) 
        {
            GValue *filter_value;
            GType property_type = G_VALUE_TYPE (property_value);

            if (property_type == G_TYPE_BOOLEAN ||
                property_type == G_TYPE_STRING ||
                property_type == DBUS_TYPE_G_OBJECT_PATH)
            {
                filter_value = tp_g_value_slice_new
                    (G_VALUE_TYPE (property_value));
                g_value_copy (property_value, filter_value);
            }
            else if (property_type == G_TYPE_UCHAR ||
                     property_type == G_TYPE_UINT ||
                     property_type == G_TYPE_UINT64)
            {
                filter_value = tp_g_value_slice_new (G_TYPE_UINT64);
                g_value_transform (property_value, filter_value);
            }
            else if (property_type == G_TYPE_INT ||
                     property_type == G_TYPE_INT64)
            {
                filter_value = tp_g_value_slice_new (G_TYPE_INT64);
                g_value_transform (property_value, filter_value);
            }
            else
            {
                /* invalid type, do not add this filter */
                g_warning ("%s: Property %s has an invalid type (%s)",
                           G_STRFUNC, property_name,
                           g_type_name (G_VALUE_TYPE (property_value)));
                valid_filter = FALSE;
                break;
            }

            g_hash_table_insert (new_channel_class, g_strdup (property_name),
                                 filter_value);
        }

        if (valid_filter)
            client_filters = g_list_prepend (client_filters,
                                             new_channel_class);
        else
            g_hash_table_destroy (new_channel_class);
    }

    switch (interface)
    {
        case MCD_CLIENT_OBSERVER:
            _mcd_client_proxy_take_observer_filters (client,
                                                     client_filters);
            break;

        case MCD_CLIENT_APPROVER:
            _mcd_client_proxy_take_approver_filters (client,
                                                     client_filters);
            break;

        case MCD_CLIENT_HANDLER:
            _mcd_client_proxy_take_handler_filters (client,
                                                    client_filters);
            break;

        default:
            g_assert_not_reached ();
    }
}

/* This is NULL-safe for the last argument, for ease of use with
 * tp_asv_get_boxed */
void
_mcd_client_proxy_add_cap_tokens (McdClientProxy *self,
                                  const gchar * const *cap_tokens)
{
    guint i;

    g_return_if_fail (MCD_IS_CLIENT_PROXY (self));

    if (cap_tokens == NULL)
        return;

    for (i = 0; cap_tokens[i] != NULL; i++)
    {
        TpHandle handle = tp_handle_ensure (self->priv->string_pool,
                                            cap_tokens[i], NULL, NULL);

        tp_handle_set_add (self->priv->capability_tokens, handle);
        tp_handle_unref (self->priv->string_pool, handle);
    }
}

void
_mcd_client_proxy_add_interfaces (McdClientProxy *self,
                                  const gchar * const *interfaces)
{
    guint i;

    if (interfaces == NULL)
        return;

    for (i = 0; interfaces[i] != NULL; i++)
    {
        if (tp_dbus_check_valid_interface_name (interfaces[i], NULL))
        {
            GQuark q = g_quark_from_string (interfaces[i]);

            DEBUG ("%s: %s", tp_proxy_get_bus_name (self), interfaces[i]);
            tp_proxy_add_interface_by_id ((TpProxy *) self, q);
        }
    }
}

static void
_mcd_client_proxy_init (McdClientProxy *self)
{
    self->priv = G_TYPE_INSTANCE_GET_PRIVATE (self, MCD_TYPE_CLIENT_PROXY,
                                              McdClientProxyPrivate);
}

gboolean
_mcd_client_proxy_is_ready (McdClientProxy *self)
{
    g_return_val_if_fail (MCD_IS_CLIENT_PROXY (self), FALSE);

    return self->priv->ready;
}

gboolean
_mcd_client_proxy_is_active (McdClientProxy *self)
{
    g_return_val_if_fail (MCD_IS_CLIENT_PROXY (self), FALSE);
    g_return_val_if_fail (self->priv->ready, FALSE);

    return self->priv->unique_name != NULL &&
        self->priv->unique_name[0] != '\0';
}

gboolean
_mcd_client_proxy_is_activatable (McdClientProxy *self)
{
    g_return_val_if_fail (MCD_IS_CLIENT_PROXY (self), FALSE);
    g_return_val_if_fail (self->priv->ready, FALSE);

    return self->priv->activatable;
}

const gchar *
_mcd_client_proxy_get_unique_name (McdClientProxy *self)
{
    g_return_val_if_fail (MCD_IS_CLIENT_PROXY (self), NULL);
    g_return_val_if_fail (self->priv->ready, NULL);

    return self->priv->unique_name;
}

static void
mcd_client_proxy_emit_ready (McdClientProxy *self)
{
    if (self->priv->ready)
        return;

    self->priv->ready = TRUE;

    g_signal_emit (self, signals[S_READY], 0);
}

gboolean
_mcd_client_proxy_parse_client_file (McdClientProxy *self)
{
    gboolean file_found = FALSE;
    gchar *filename;
    const gchar *bus_name = tp_proxy_get_bus_name (self);

    filename = _mcd_client_proxy_find_client_file (
        bus_name + MC_CLIENT_BUS_NAME_BASE_LEN);

    if (filename)
    {
        GKeyFile *file;
        GError *error = NULL;

        file = g_key_file_new ();
        g_key_file_load_from_file (file, filename, 0, &error);
        if (G_LIKELY (!error))
        {
            DEBUG ("File found for %s: %s", bus_name, filename);
            parse_client_file (self, file);
            file_found = TRUE;
        }
        else
        {
            g_warning ("Loading file %s failed: %s", filename, error->message);
            g_error_free (error);
        }
        g_key_file_free (file);
        g_free (filename);
    }

    return file_found;
}

static gboolean
mcd_client_proxy_introspect (gpointer data)
{
    mcd_client_proxy_emit_ready (data);
    return FALSE;
}

static void
mcd_client_proxy_unique_name_cb (TpDBusDaemon *dbus_daemon,
                                 const gchar *unique_name,
                                 const GError *error,
                                 gpointer unused G_GNUC_UNUSED,
                                 GObject *weak_object)
{
    McdClientProxy *self = MCD_CLIENT_PROXY (weak_object);

    if (error != NULL)
    {
        DEBUG ("Error getting unique name, assuming not active: %s %d: %s",
               g_quark_to_string (error->domain), error->code, error->message);
        _mcd_client_proxy_set_inactive (self);
    }
    else
    {
        _mcd_client_proxy_set_active (self, unique_name);
    }

    mcd_client_proxy_introspect (self);
}

static void
mcd_client_proxy_dispose (GObject *object)
{
    McdClientProxy *self = MCD_CLIENT_PROXY (object);
    void (*chain_up) (GObject *) =
        ((GObjectClass *) _mcd_client_proxy_parent_class)->dispose;

    if (self->priv->string_pool != NULL)
    {
        if (self->priv->capability_tokens != NULL)
        {
            tp_handle_set_destroy (self->priv->capability_tokens);
            self->priv->capability_tokens = NULL;
        }

        g_object_unref (self->priv->string_pool);
        self->priv->string_pool = NULL;
    }

    if (chain_up != NULL)
    {
        chain_up (object);
    }
}

static void
mcd_client_proxy_finalize (GObject *object)
{
    McdClientProxy *self = MCD_CLIENT_PROXY (object);
    void (*chain_up) (GObject *) =
        ((GObjectClass *) _mcd_client_proxy_parent_class)->finalize;

    g_free (self->priv->unique_name);

    _mcd_client_proxy_take_approver_filters (self, NULL);
    _mcd_client_proxy_take_observer_filters (self, NULL);
    _mcd_client_proxy_take_handler_filters (self, NULL);

    if (chain_up != NULL)
    {
        chain_up (object);
    }
}

static void
mcd_client_proxy_constructed (GObject *object)
{
    McdClientProxy *self = MCD_CLIENT_PROXY (object);
    void (*chain_up) (GObject *) =
        ((GObjectClass *) _mcd_client_proxy_parent_class)->constructed;
    const gchar *bus_name;

    if (chain_up != NULL)
    {
        chain_up (object);
    }

    bus_name = tp_proxy_get_bus_name (self);

    self->priv->capability_tokens = tp_handle_set_new (
        self->priv->string_pool);

    DEBUG ("%s", bus_name);

    if (self->priv->unique_name == NULL)
    {
        tp_cli_dbus_daemon_call_get_name_owner (tp_proxy_get_dbus_daemon (self),
                                                -1,
                                                bus_name,
                                                mcd_client_proxy_unique_name_cb,
                                                NULL, NULL, (GObject *) self);
    }
    else
    {
        g_idle_add_full (G_PRIORITY_HIGH, mcd_client_proxy_introspect,
                         g_object_ref (self), g_object_unref);
    }
}

static void
mcd_client_proxy_set_property (GObject *object,
                               guint property,
                               const GValue *value,
                               GParamSpec *param_spec)
{
    McdClientProxy *self = MCD_CLIENT_PROXY (object);

    switch (property)
    {
        case PROP_ACTIVATABLE:
            self->priv->activatable = g_value_get_boolean (value);
            break;

        case PROP_STRING_POOL:
            g_assert (self->priv->string_pool == NULL);
            self->priv->string_pool = g_value_dup_object (value);
            break;

        case PROP_UNIQUE_NAME:
            g_assert (self->priv->unique_name == NULL);
            self->priv->unique_name = g_value_dup_string (value);
            break;

        default:
            G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property, param_spec);
    }
}

static void
_mcd_client_proxy_class_init (McdClientProxyClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);

    g_type_class_add_private (object_class, sizeof (McdClientProxyPrivate));

    object_class->constructed = mcd_client_proxy_constructed;
    object_class->dispose = mcd_client_proxy_dispose;
    object_class->finalize = mcd_client_proxy_finalize;
    object_class->set_property = mcd_client_proxy_set_property;

    signals[S_READY] = g_signal_new ("ready", G_OBJECT_CLASS_TYPE (klass),
                                     G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
                                     0, NULL, NULL,
                                     g_cclosure_marshal_VOID__VOID,
                                     G_TYPE_NONE, 0);

    g_object_class_install_property (object_class, PROP_ACTIVATABLE,
        g_param_spec_boolean ("activatable", "Activatable?",
            "TRUE if this client can be service-activated", FALSE,
            G_PARAM_WRITABLE | G_PARAM_CONSTRUCT | G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (object_class, PROP_STRING_POOL,
        g_param_spec_object ("string-pool", "String pool",
            "TpHandleRepoIface used to intern strings representing capability "
            "tokens",
            G_TYPE_OBJECT,
            G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
            G_PARAM_STATIC_STRINGS));

    g_object_class_install_property (object_class, PROP_UNIQUE_NAME,
        g_param_spec_string ("unique-name", "Unique name",
            "The D-Bus unique name of this client, \"\" if not running or "
            "NULL if unknown",
            NULL,
            G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
            G_PARAM_STATIC_STRINGS));

}

gboolean
_mcd_client_check_valid_name (const gchar *name_suffix,
                              GError **error)
{
    guint i;

    if (!g_ascii_isalpha (*name_suffix))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Client names must start with a letter");
        return FALSE;
    }

    for (i = 1; name_suffix[i] != '\0'; i++)
    {
        if (i > (255 - MC_CLIENT_BUS_NAME_BASE_LEN))
        {
            g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                         "Client name too long");
        }

        if (name_suffix[i] == '_' || g_ascii_isalpha (name_suffix[i]))
        {
            continue;
        }

        if (name_suffix[i] == '.' || g_ascii_isdigit (name_suffix[i]))
        {
            if (name_suffix[i-1] == '.')
            {
                g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                             "Client names must not have a digit or dot "
                             "following a dot");
                return FALSE;
            }
        }
        else
        {
            g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                         "Client names must not contain '%c'", name_suffix[i]);
            return FALSE;
        }
    }

    if (name_suffix[i-1] == '.')
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Client names must not end with a dot");
        return FALSE;
    }

    return TRUE;
}

McdClientProxy *
_mcd_client_proxy_new (TpDBusDaemon *dbus_daemon,
                       TpHandleRepoIface *string_pool,
                       const gchar *well_known_name,
                       const gchar *unique_name_if_known,
                       gboolean activatable)
{
    McdClientProxy *self;
    const gchar *name_suffix;
    gchar *object_path;

    g_return_val_if_fail (g_str_has_prefix (well_known_name,
                                            TP_CLIENT_BUS_NAME_BASE), NULL);
    name_suffix = well_known_name + MC_CLIENT_BUS_NAME_BASE_LEN;
    g_return_val_if_fail (_mcd_client_check_valid_name (name_suffix, NULL),
                          NULL);

    object_path = g_strconcat ("/", well_known_name, NULL);
    g_strdelimit (object_path, ".", '/');

    g_assert (tp_dbus_check_valid_bus_name (well_known_name,
                                            TP_DBUS_NAME_TYPE_WELL_KNOWN,
                                            NULL));
    g_assert (tp_dbus_check_valid_object_path (object_path, NULL));

    self = g_object_new (MCD_TYPE_CLIENT_PROXY,
                         "dbus-daemon", dbus_daemon,
                         "string-pool", string_pool,
                         "object-path", object_path,
                         "bus-name", well_known_name,
                         "unique-name", unique_name_if_known,
                         "activatable", activatable,
                         NULL);

    g_free (object_path);

    return self;
}

void
_mcd_client_proxy_set_inactive (McdClientProxy *self)
{
    g_return_if_fail (MCD_IS_CLIENT_PROXY (self));

    g_free (self->priv->unique_name);
    self->priv->unique_name = g_strdup ("");
}

void
_mcd_client_proxy_set_active (McdClientProxy *self,
                              const gchar *unique_name)
{
    g_return_if_fail (MCD_IS_CLIENT_PROXY (self));

    g_free (self->priv->unique_name);
    self->priv->unique_name = g_strdup (unique_name);
}

void
_mcd_client_proxy_set_activatable (McdClientProxy *self)
{
    g_return_if_fail (MCD_IS_CLIENT_PROXY (self));

    self->priv->activatable = TRUE;
}

const GList *
_mcd_client_proxy_get_approver_filters (McdClientProxy *self)
{
    g_return_val_if_fail (MCD_IS_CLIENT_PROXY (self), NULL);

    return self->priv->approver_filters;
}

const GList *
_mcd_client_proxy_get_observer_filters (McdClientProxy *self)
{
    g_return_val_if_fail (MCD_IS_CLIENT_PROXY (self), NULL);

    return self->priv->observer_filters;
}

const GList *
_mcd_client_proxy_get_handler_filters (McdClientProxy *self)
{
    g_return_val_if_fail (MCD_IS_CLIENT_PROXY (self), NULL);

    return self->priv->handler_filters;
}

static void
mcd_client_proxy_free_client_filters (GList **client_filters)
{
    g_assert (client_filters != NULL);

    if (*client_filters != NULL)
    {
        g_list_foreach (*client_filters, (GFunc) g_hash_table_destroy, NULL);
        g_list_free (*client_filters);
        *client_filters = NULL;
    }
}

void
_mcd_client_proxy_take_approver_filters (McdClientProxy *self,
                                         GList *filters)
{
    g_return_if_fail (MCD_IS_CLIENT_PROXY (self));

    mcd_client_proxy_free_client_filters (&(self->priv->approver_filters));
    self->priv->approver_filters = filters;
}

void
_mcd_client_proxy_take_observer_filters (McdClientProxy *self,
                                         GList *filters)
{
    g_return_if_fail (MCD_IS_CLIENT_PROXY (self));

    mcd_client_proxy_free_client_filters (&(self->priv->observer_filters));
    self->priv->observer_filters = filters;
}

void
_mcd_client_proxy_take_handler_filters (McdClientProxy *self,
                                        GList *filters)
{
    g_return_if_fail (MCD_IS_CLIENT_PROXY (self));

    mcd_client_proxy_free_client_filters (&(self->priv->handler_filters));
    self->priv->handler_filters = filters;
}

gboolean
_mcd_client_proxy_get_bypass_approval (McdClientProxy *self)
{
    g_return_val_if_fail (MCD_IS_CLIENT_PROXY (self), FALSE);

    return self->priv->bypass_approval;
}

void
_mcd_client_proxy_set_bypass_approval (McdClientProxy *self,
                                       gboolean bypass)
{
    g_return_if_fail (MCD_IS_CLIENT_PROXY (self));

    self->priv->bypass_approval = bypass;
}

void
_mcd_client_proxy_become_incapable (McdClientProxy *self)
{
    _mcd_client_proxy_take_approver_filters (self, NULL);
    _mcd_client_proxy_take_observer_filters (self, NULL);
    _mcd_client_proxy_take_handler_filters (self, NULL);
    tp_handle_set_destroy (self->priv->capability_tokens);
    self->priv->capability_tokens = tp_handle_set_new (
        self->priv->string_pool);
}

typedef struct {
    TpHandleRepoIface *repo;
    GPtrArray *array;
} TokenAppendContext;

static void
append_token_to_ptrs (TpHandleSet *unused G_GNUC_UNUSED,
                      TpHandle handle,
                      gpointer data)
{
    TokenAppendContext *context = data;

    g_ptr_array_add (context->array,
                     g_strdup (tp_handle_inspect (context->repo, handle)));
}

GValueArray *
_mcd_client_proxy_dup_handler_capabilities (McdClientProxy *self)
{
    GPtrArray *filters;
    GPtrArray *cap_tokens;
    GValueArray *va;
    const GList *list;

    g_return_val_if_fail (MCD_IS_CLIENT_PROXY (self), NULL);

    filters = g_ptr_array_sized_new (
        g_list_length (self->priv->handler_filters));

    for (list = self->priv->handler_filters; list != NULL; list = list->next)
    {
        GHashTable *copy = g_hash_table_new_full (g_str_hash, g_str_equal,
            g_free, (GDestroyNotify) tp_g_value_slice_free);

        tp_g_hash_table_update (copy, list->data,
                                (GBoxedCopyFunc) g_strdup,
                                (GBoxedCopyFunc) tp_g_value_slice_dup);
        g_ptr_array_add (filters, copy);
    }

    if (self->priv->capability_tokens == NULL)
    {
        cap_tokens = g_ptr_array_sized_new (1);
    }
    else
    {
        TokenAppendContext context = { self->priv->string_pool, NULL };

        cap_tokens = g_ptr_array_sized_new (
            tp_handle_set_size (self->priv->capability_tokens) + 1);
        context.array = cap_tokens;
        tp_handle_set_foreach (self->priv->capability_tokens,
                               append_token_to_ptrs, &context);
    }

    g_ptr_array_add (cap_tokens, NULL);

    if (DEBUGGING)
    {
        guint i;

        DEBUG ("%s:", tp_proxy_get_bus_name (self));

        DEBUG ("- %u channel filters", filters->len);
        DEBUG ("- %u capability tokens:", cap_tokens->len - 1);

        for (i = 0; i < cap_tokens->len - 1; i++)
        {
            DEBUG ("    %s", (gchar *) g_ptr_array_index (cap_tokens, i));
        }

        DEBUG ("-end-");
    }

    va = g_value_array_new (3);
    g_value_array_append (va, NULL);
    g_value_array_append (va, NULL);
    g_value_array_append (va, NULL);

    g_value_init (va->values + 0, G_TYPE_STRING);
    g_value_init (va->values + 1, TP_ARRAY_TYPE_CHANNEL_CLASS_LIST);
    g_value_init (va->values + 2, G_TYPE_STRV);

    g_value_set_string (va->values + 0, tp_proxy_get_bus_name (self));
    g_value_take_boxed (va->values + 1, filters);
    g_value_take_boxed (va->values + 2, g_ptr_array_free (cap_tokens, FALSE));

    return va;
}
