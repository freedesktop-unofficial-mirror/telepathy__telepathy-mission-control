/* vi: set et sw=4 ts=8 cino=t0,(0: */
/* -*- Mode: C; indent-tabs-mode: nil; c-basic-offset: 4; tab-width: 8 -*- */
/*
 * This file is part of mission-control
 *
 * Copyright (C) 2008-2010 Nokia Corporation.
 * Copyright (C) 2009-2010 Collabora Ltd.
 *
 * Contact: Alberto Mardegan  <alberto.mardegan@nokia.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
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
#include "mcd-account.h"
#include "mcd-storage-priv.h"

#include <stdio.h>
#include <string.h>

#include <dbus/dbus.h>
#include <glib/gi18n.h>
#include <glib/gstdio.h>
#include <telepathy-glib/gtypes.h>
#include <telepathy-glib/interfaces.h>
#include <telepathy-glib/svc-account.h>
#include <telepathy-glib/svc-generic.h>
#include <telepathy-glib/util.h>

#include <libmcclient/mc-gtypes.h>
#include <libmcclient/mc-interfaces.h>

#include "mcd-account-priv.h"
#include "mcd-account-compat.h"
#include "mcd-account-conditions.h"
#include "mcd-account-manager-priv.h"
#include "mcd-account-addressing.h"
#include "mcd-connection-plugin.h"
#include "mcd-connection-priv.h"
#include "mcd-misc.h"
#include "mcd-signals-marshal.h"
#include "mcd-manager.h"
#include "mcd-manager-priv.h"
#include "mcd-master.h"
#include "mcd-master-priv.h"
#include "mcd-dbusprop.h"

#define MAX_KEY_LENGTH (DBUS_MAXIMUM_NAME_LENGTH + 6)
#define MC_AVATAR_FILENAME	"avatar.bin"

#define MCD_ACCOUNT_PRIV(account) (MCD_ACCOUNT (account)->priv)

static void account_iface_init (TpSvcAccountClass *iface,
			       	gpointer iface_data);
static void properties_iface_init (TpSvcDBusPropertiesClass *iface,
				   gpointer iface_data);
static void account_avatar_iface_init (TpSvcAccountInterfaceAvatarClass *iface,
				       gpointer iface_data);
static void account_storage_iface_init (
    TpSvcAccountInterfaceStorageClass *iface,
    gpointer iface_data);
static void account_hidden_iface_init (
    McSvcAccountInterfaceHiddenClass *iface,
    gpointer iface_data);

static const McdDBusProp account_properties[];
static const McdDBusProp account_avatar_properties[];
static const McdDBusProp account_storage_properties[];
static const McdDBusProp account_hidden_properties[];

static const McdInterfaceData account_interfaces[] = {
    MCD_IMPLEMENT_IFACE (tp_svc_account_get_type, account, TP_IFACE_ACCOUNT),
    MCD_IMPLEMENT_IFACE (tp_svc_account_interface_avatar_get_type,
			 account_avatar,
			 TP_IFACE_ACCOUNT_INTERFACE_AVATAR),
    MCD_IMPLEMENT_IFACE (mc_svc_account_interface_channelrequests_get_type,
			 account_channelrequests,
			 MC_IFACE_ACCOUNT_INTERFACE_CHANNELREQUESTS),
    MCD_IMPLEMENT_IFACE (mc_svc_account_interface_compat_get_type,
			 account_compat,
			 MC_IFACE_ACCOUNT_INTERFACE_COMPAT),
    MCD_IMPLEMENT_IFACE (mc_svc_account_interface_conditions_get_type,
			 account_conditions,
			 MC_IFACE_ACCOUNT_INTERFACE_CONDITIONS),
    MCD_IMPLEMENT_IFACE (tp_svc_account_interface_storage_get_type,
                         account_storage,
                         TP_IFACE_ACCOUNT_INTERFACE_STORAGE),
    MCD_IMPLEMENT_IFACE_WITH_INIT (mc_svc_account_interface_stats_get_type,
                                   account_stats,
                                   MC_IFACE_ACCOUNT_INTERFACE_STATS),
    MCD_IMPLEMENT_IFACE (mc_svc_account_interface_addressing_get_type,
        account_addressing,
        MC_IFACE_ACCOUNT_INTERFACE_ADDRESSING),
    MCD_IMPLEMENT_IFACE (mc_svc_account_interface_hidden_get_type,
                         account_hidden,
                         MC_IFACE_ACCOUNT_INTERFACE_HIDDEN),

    { G_TYPE_INVALID, }
};

G_DEFINE_TYPE_WITH_CODE (McdAccount, mcd_account, G_TYPE_OBJECT,
			 MCD_DBUS_INIT_INTERFACES (account_interfaces);
			 G_IMPLEMENT_INTERFACE (TP_TYPE_SVC_DBUS_PROPERTIES,
						properties_iface_init);
			)

struct _McdAccountPrivate
{
    gchar *unique_name;
    gchar *object_path;
    gchar *manager_name;
    gchar *protocol_name;

    TpConnection *tp_connection;
    McdConnection *connection;
    McdManager *manager;

    McdStorage *storage;
    TpDBusDaemon *dbus_daemon;

    McdTransport *transport;
    McdAccountConnectionContext *connection_context;
    GKeyFile *keyfile;		/* configuration file */
    McpAccountStorage *storage_plugin;

    /* connection status */
    TpConnectionStatus conn_status;
    TpConnectionStatusReason conn_reason;
    gchar *conn_dbus_error;
    GHashTable *conn_error_details;

    /* current presence fields */
    TpConnectionPresenceType curr_presence_type;
    gchar *curr_presence_status;
    gchar *curr_presence_message;

    /* requested presence fields */
    TpConnectionPresenceType req_presence_type;
    gchar *req_presence_status;
    gchar *req_presence_message;

    /* automatic presence fields */
    TpConnectionPresenceType auto_presence_type;
    gchar *auto_presence_status; /* TODO: consider loading these from the
				    configuration file as needed */
    gchar *auto_presence_message;

    GList *online_requests; /* list of McdOnlineRequestData structures
                               (callback with user data) to be called when the
                               account will be online */

    guint connect_automatically : 1;
    guint enabled : 1;
    guint valid : 1;
    guint loaded : 1;
    guint has_been_online : 1;
    guint removed : 1;
    guint always_on : 1;
    guint changing_presence : 1;

    gboolean hidden;

    /* These fields are used to cache the changed properties */
    gboolean properties_frozen;
    GHashTable *changed_properties;
    guint properties_source;
};

enum
{
    PROP_0,
    PROP_DBUS_DAEMON,
    PROP_STORAGE,
    PROP_NAME,
    PROP_ALWAYS_ON,
    PROP_HIDDEN,
};

enum
{
    CONNECTION_STATUS_CHANGED,
    VALIDITY_CHANGED,
    LAST_SIGNAL
};

static guint _mcd_account_signals[LAST_SIGNAL] = { 0 };
static GQuark account_ready_quark = 0;

GQuark
mcd_account_error_quark (void)
{
    static GQuark quark = 0;

    if (quark == 0)
        quark = g_quark_from_static_string ("mcd-account-error");

    return quark;
}

/*
 * _mcd_account_maybe_autoconnect:
 * @account: the #McdAccount.
 *
 * Check whether automatic connection should happen (and attempt it if needed).
 */
void
_mcd_account_maybe_autoconnect (McdAccount *account)
{
    McdAccountPrivate *priv;
    McdMaster *master;

    g_return_if_fail (MCD_IS_ACCOUNT (account));
    priv = account->priv;

    if (!priv->enabled)
    {
        DEBUG ("%s not Enabled", priv->unique_name);
        return;
    }

    if (!priv->valid)
    {
        DEBUG ("%s not Valid", priv->unique_name);
        return;
    }

    if (priv->conn_status != TP_CONNECTION_STATUS_DISCONNECTED)
    {
        DEBUG ("%s already connecting/connected", priv->unique_name);
        return;
    }

    if (!priv->connect_automatically)
    {
        DEBUG ("%s does not ConnectAutomatically", priv->unique_name);
        return;
    }

    master = mcd_master_get_default ();

    if (!_mcd_master_account_replace_transport (master, account))
    {
        DEBUG ("%s conditions not satisfied", priv->unique_name);
        return;
    }

    DEBUG ("connecting account %s", priv->unique_name);
    _mcd_account_connect_with_auto_presence (account);
}

static gboolean
value_is_same (const GValue *val1, const GValue *val2)
{
    g_return_val_if_fail (val1 != NULL && val2 != NULL, FALSE);
    switch (G_VALUE_TYPE (val1))
    {
    case G_TYPE_STRING:
        return g_strcmp0 (g_value_get_string (val1),
                          g_value_get_string (val2)) == 0;
    case G_TYPE_CHAR:
    case G_TYPE_UCHAR:
    case G_TYPE_INT:
    case G_TYPE_UINT:
    case G_TYPE_BOOLEAN:
        return val1->data[0].v_uint == val2->data[0].v_uint;

    case G_TYPE_INT64:
        return g_value_get_int64 (val1) == g_value_get_int64 (val2);
    case G_TYPE_UINT64:
        return g_value_get_uint64 (val1) == g_value_get_uint64 (val2);

    case G_TYPE_DOUBLE:
        return g_value_get_double (val1) == g_value_get_double (val2);

    default:
        if (G_VALUE_TYPE (val1) == DBUS_TYPE_G_OBJECT_PATH)
        {
            return !tp_strdiff (g_value_get_boxed (val1),
                                g_value_get_boxed (val2));
        }
        else if (G_VALUE_TYPE (val1) == G_TYPE_STRV)
        {
            gchar **left = g_value_get_boxed (val1);
            gchar **right = g_value_get_boxed (val2);

            if (left == NULL || right == NULL ||
                *left == NULL || *right == NULL)
            {
                return ((left == NULL || *left == NULL) &&
                        (right == NULL || *right == NULL));
            }

            while (*left != NULL || *right != NULL)
            {
                if (tp_strdiff (*left, *right))
                {
                    return FALSE;
                }

                left++;
                right++;
            }

            return TRUE;
        }
        else
        {
            g_warning ("%s: unexpected type %s",
                       G_STRFUNC, G_VALUE_TYPE_NAME (val1));
            return FALSE;
        }
    }
}

static void
mcd_account_loaded (McdAccount *account)
{
    g_return_if_fail (!account->priv->loaded);
    account->priv->loaded = TRUE;

    /* invoke all the callbacks */
    g_object_ref (account);

    _mcd_object_ready (account, account_ready_quark, NULL);

    if (account->priv->online_requests != NULL)
    {
        /* if we have established that the account is not valid or is
         * disabled, cancel all requests */
        if (!account->priv->valid || !account->priv->enabled)
        {
            /* FIXME: pick better errors and put them in telepathy-spec? */
            GError e = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
                "account isn't Valid (not enough information to put it "
                    "online)" };
            GList *list;

            if (account->priv->valid)
            {
                e.message = "account isn't Enabled";
            }

            list = account->priv->online_requests;
            account->priv->online_requests = NULL;

            for (/* already initialized */ ;
                 list != NULL;
                 list = g_list_delete_link (list, list))
            {
                McdOnlineRequestData *data = list->data;

                data->callback (account, data->user_data, &e);
                g_slice_free (McdOnlineRequestData, data);
            }
        }

        /* otherwise, we want to go online now */
        if (account->priv->conn_status == TP_CONNECTION_STATUS_DISCONNECTED)
        {
            _mcd_account_connect_with_auto_presence (account);
        }
    }

    _mcd_account_maybe_autoconnect (account);

    g_object_unref (account);
}

/*
 * _mcd_account_set_parameter:
 * @account: the #McdAccount.
 * @name: the parameter name.
 * @value: a #GValue with the value to set, or %NULL.
 *
 * Sets the parameter @name to the value in @value. If @value, is %NULL, the
 * parameter is unset.
 */
static void
_mcd_account_set_parameter (McdAccount *account, const gchar *name,
                            const GValue *value)
{
    McdAccountPrivate *priv = account->priv;
    McdStorage *storage = priv->storage;
    gchar key[MAX_KEY_LENGTH];
    const gchar *account_name = mcd_account_get_unique_name (account);
    gboolean secret = mcd_account_parameter_is_secret (account, name);

    g_snprintf (key, sizeof (key), "param-%s", name);

    mcd_storage_set_value (storage, account_name, key, value, secret);
}












static GType mc_param_type (const TpConnectionManagerParam *param);

/**
 * mcd_account_get_parameter:
 * @account: the #McdAccount.
 * @name: the parameter name.
 * @parameter: location at which to store the parameter's current value, or
 *  %NULL if you don't actually care about the parameter's value.
 * @error: location at which to store an error if the parameter cannot be
 *  retrieved.
 *
 * Get the @name parameter for @account.
 *
 * Returns: %TRUE if the parameter could be retrieved; %FALSE otherwise
 */
static gboolean
mcd_account_get_parameter (McdAccount *account, const gchar *name,
                           GValue *parameter,
                           GError **error)
{
    McdAccountPrivate *priv = account->priv;
    McdStorage *storage = priv->storage;
    gchar key[MAX_KEY_LENGTH];
    const TpConnectionManagerParam *param;
    GType type;
    const gchar *account_name = mcd_account_get_unique_name (account);

    param = mcd_manager_get_protocol_param (priv->manager,
                                            priv->protocol_name, name);
    type = mc_param_type (param);

    g_snprintf (key, sizeof (key), "param-%s", name);

    if (mcd_storage_has_value (storage, account_name, key))
    {
        GError *error2 = NULL;
        GValue *value = mcd_storage_dup_value (storage, account_name, key,
            type, &error2);

        if (value != NULL)
        {
            if (error2 != NULL)
            {
                DEBUG ("type mismatch for parameter '%s': %s", name,
                       error2->message);
                DEBUG ("using default");
                g_clear_error (&error2);
            }

            if (parameter != NULL)
            {
                g_value_init (parameter, type);
                g_value_copy (value, parameter);
            }

            tp_g_value_slice_free (value);
            return TRUE;
        }
        else
        {
            g_propagate_error (error, error2);
            return FALSE;
        }
    }
    else
    {
        g_set_error (error, MCD_ACCOUNT_ERROR,
                     MCD_ACCOUNT_ERROR_GET_PARAMETER,
                     "Keyfile does not have key %s", key);
        return FALSE;
    }
}


typedef void (*CheckParametersCb) (McdAccount *account, gboolean valid,
                                   gpointer user_data);
static void mcd_account_check_parameters (McdAccount *account,
    CheckParametersCb callback, gpointer user_data);

static void
manager_ready_check_params_cb (McdAccount *account,
    gboolean valid,
    gpointer user_data)
{
    McdAccountPrivate *priv = account->priv;

    priv->valid = valid;
    mcd_account_loaded (account);
}

static void on_manager_ready (McdManager *manager, const GError *error,
                              gpointer user_data)
{
    McdAccount *account = MCD_ACCOUNT (user_data);

    if (error)
    {
        DEBUG ("got error: %s", error->message);
        mcd_account_loaded (account);
    }
    else
    {
        mcd_account_check_parameters (account, manager_ready_check_params_cb,
                                      NULL);
    }
}

static gboolean
load_manager (McdAccount *account)
{
    McdAccountPrivate *priv = account->priv;
    McdMaster *master;

    if (G_UNLIKELY (!priv->manager_name)) return FALSE;
    master = mcd_master_get_default ();
    priv->manager = _mcd_master_lookup_manager (master, priv->manager_name);
    if (priv->manager)
    {
	g_object_ref (priv->manager);
        mcd_manager_call_when_ready (priv->manager, on_manager_ready, account);
	return TRUE;
    }
    else
	return FALSE;
}

/* Returns the data dir for the given account name.
 * Returned string must be freed by caller. */
static gchar *
get_account_data_path (McdAccountPrivate *priv)
{
    const gchar *base;

    base = g_getenv ("MC_ACCOUNT_DIR");
    if (!base)
	base = ACCOUNTS_DIR;
    if (!base)
	return NULL;

    if (base[0] == '~')
	return g_build_filename (g_get_home_dir(), base + 1,
				 priv->unique_name, NULL);
    else
	return g_build_filename (base, priv->unique_name, NULL);
}

void
mcd_account_delete (McdAccount *account,
                     McdAccountDeleteCb callback,
                     gpointer user_data)
{
    McdAccountPrivate *priv = account->priv;
    gchar *data_dir_str;
    GError *error = NULL;
    const gchar *name = mcd_account_get_unique_name (account);

    /* got to turn the account off before removing it, otherwise we can *
     * end up with an orphaned CM holding the account online            */
    if (!_mcd_account_set_enabled (account, FALSE, FALSE, &error))
    {
        g_warning ("could not disable account %s (%s)", name, error->message);
        callback (account, error, user_data);
        g_error_free (error);
        return;
    }

    mcd_storage_delete_account (priv->storage, name);

    data_dir_str = get_account_data_path (priv);

    if (data_dir_str != NULL)
    {
        GDir *data_dir = g_dir_open (data_dir_str, 0, NULL);

        if (data_dir)
        {
            const gchar *filename;

            while ((filename = g_dir_read_name (data_dir)) != NULL)
            {
                gchar *path = g_build_filename (data_dir_str, filename, NULL);

                g_remove (path);
                g_free (path);
            }

            g_dir_close (data_dir);
            g_rmdir (data_dir_str);
        }

        g_free (data_dir_str);
    }

    mcd_storage_commit (priv->storage, name);
    if (callback != NULL)
        callback (account, NULL, user_data);
}

void
_mcd_account_load (McdAccount *account, McdAccountLoadCb callback,
                   gpointer user_data)
{
    if (account->priv->loaded)
        callback (account, NULL, user_data);
    else
        _mcd_object_call_when_ready (account, account_ready_quark,
                                     (McdReadyCb)callback, user_data);
}

static void
on_connection_abort (McdConnection *connection, McdAccount *account)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);

    DEBUG ("called (%p, account %s)", connection, priv->unique_name);
    _mcd_account_set_connection (account, NULL);
}

static gboolean
mcd_account_request_presence_int (McdAccount *account,
				  TpConnectionPresenceType type,
				  const gchar *status, const gchar *message)
{
    McdAccountPrivate *priv = account->priv;
    gboolean changed = FALSE;

    if (priv->req_presence_type != type)
    {
        priv->req_presence_type = type;
        changed = TRUE;
    }

    if (tp_strdiff (priv->req_presence_status, status))
    {
        g_free (priv->req_presence_status);
        priv->req_presence_status = g_strdup (status);
        changed = TRUE;
    }

    if (tp_strdiff (priv->req_presence_message, message))
    {
        g_free (priv->req_presence_message);
        priv->req_presence_message = g_strdup (message);
        changed = TRUE;
    }

    DEBUG ("Requested presence: %u %s %s",
        priv->req_presence_type,
        priv->req_presence_status,
        priv->req_presence_message);

    if (type >= TP_CONNECTION_PRESENCE_TYPE_AVAILABLE)
    {
        if (!priv->enabled)
        {
            DEBUG ("%s not Enabled", priv->unique_name);
            return changed;
        }

        if (!priv->valid)
        {
            DEBUG ("%s not Valid", priv->unique_name);
            return changed;
        }
    }

    if (changed)
    {
        _mcd_account_set_changing_presence (account, TRUE);
    }

    if (priv->connection == NULL)
    {
        if (type >= TP_CONNECTION_PRESENCE_TYPE_AVAILABLE)
        {
            _mcd_account_connection_begin (account);
        }
    }
    else
    {
        _mcd_connection_request_presence (priv->connection,
                                          priv->req_presence_type,
					  priv->req_presence_status,
					  priv->req_presence_message);
    }

    return changed;
}

void
_mcd_account_connect (McdAccount *account, GHashTable *params)
{
    McdAccountPrivate *priv = account->priv;

    g_assert (params != NULL);

    if (!priv->connection)
    {
        McdConnection *connection;

	if (!priv->manager && !load_manager (account))
	{
	    g_warning ("%s: Could not find manager `%s'",
		       G_STRFUNC, priv->manager_name);
	    return;
	}

        connection = mcd_manager_create_connection (priv->manager, account);
        _mcd_account_set_connection (account, connection);
    }
    _mcd_connection_connect (priv->connection, params);
}

static gboolean
emit_property_changed (gpointer userdata)
{
    McdAccount *account = MCD_ACCOUNT (userdata);
    McdAccountPrivate *priv = account->priv;

    DEBUG ("called");

    if (g_hash_table_size (priv->changed_properties) > 0)
    {
        tp_svc_account_emit_account_property_changed (account,
            priv->changed_properties);
        g_hash_table_remove_all (priv->changed_properties);
    }

    if (priv->properties_source != 0)
    {
      g_source_remove (priv->properties_source);
      priv->properties_source = 0;
    }
    return FALSE;
}

static void
mcd_account_freeze_properties (McdAccount *self)
{
    g_return_if_fail (!self->priv->properties_frozen);
    DEBUG ("%s", self->priv->unique_name);
    self->priv->properties_frozen = TRUE;
}

static void
mcd_account_thaw_properties (McdAccount *self)
{
    g_return_if_fail (self->priv->properties_frozen);
    DEBUG ("%s", self->priv->unique_name);
    self->priv->properties_frozen = FALSE;

    if (g_hash_table_size (self->priv->changed_properties) != 0)
    {
        emit_property_changed (self);
    }
}

/*
 * This function is responsible of emitting the AccountPropertyChanged signal.
 * One possible improvement would be to save the HashTable and have the signal
 * emitted in an idle function (or a timeout function with a very small delay)
 * to group together several property changes that occur at the same time.
 */
static void
mcd_account_changed_property (McdAccount *account, const gchar *key,
			      const GValue *value)
{
    McdAccountPrivate *priv = account->priv;

    DEBUG ("called: %s", key);
    if (priv->changed_properties &&
	g_hash_table_lookup (priv->changed_properties, key))
    {
	/* the changed property was also changed before; then let's force the
	 * emission of the signal now, so that the property will appear in two
	 * separate signals */
        DEBUG ("Forcibly emit PropertiesChanged now");
	emit_property_changed (account);
    }

    if (priv->properties_source == 0)
    {
        DEBUG ("First changed property");
        priv->properties_source = g_timeout_add_full (G_PRIORITY_DEFAULT, 10,
                                                      emit_property_changed,
                                                      g_object_ref (account),
                                                      g_object_unref);
    }
    g_hash_table_insert (priv->changed_properties, (gpointer) key,
                         tp_g_value_slice_dup (value));
}

typedef enum {
    SET_RESULT_ERROR,
    SET_RESULT_UNCHANGED,
    SET_RESULT_CHANGED
} SetResult;

/*
 * mcd_account_set_string_val:
 * @account: an account
 * @key: a D-Bus property name that is a string
 * @value: the new value for that property
 * @error: set to an error if %SET_RESULT_ERROR is returned
 *
 * Returns: %SET_RESULT_CHANGED or %SET_RESULT_UNCHANGED on success,
 *  %SET_RESULT_ERROR on error
 */
static SetResult
mcd_account_set_string_val (McdAccount *account, const gchar *key,
                            const GValue *value, GError **error)
{
    McdAccountPrivate *priv = account->priv;
    McdStorage *storage = priv->storage;
    const gchar *name = mcd_account_get_unique_name (account);
    const gchar *new_string;

    if (!G_VALUE_HOLDS_STRING (value))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Expected string for %s, but got %s", key,
                     G_VALUE_TYPE_NAME (value));
        return SET_RESULT_ERROR;
    }

    new_string = g_value_get_string (value);

    if (tp_str_empty (new_string)) {
        new_string = NULL;
    }

    if (mcd_storage_set_string (storage, name, key, new_string, FALSE)) {
        mcd_storage_commit (storage, name);
        mcd_account_changed_property (account, key, value);
        return SET_RESULT_CHANGED;
    } else {
        return SET_RESULT_UNCHANGED;
    }
}

static void
mcd_account_get_string_val (McdAccount *account, const gchar *key,
			    GValue *value)
{
    McdAccountPrivate *priv = account->priv;
    const gchar *name = mcd_account_get_unique_name (account);
    GValue *fetched = NULL;

    fetched =
      mcd_storage_dup_value (priv->storage, name, key, G_TYPE_STRING, NULL);
    g_value_init (value, G_TYPE_STRING);

    if (fetched != NULL)
    {
        g_value_copy (fetched, value);
        tp_g_value_slice_free (fetched);
    }
    else
    {
        g_value_set_static_string (value, NULL);
    }
}

static gboolean
set_display_name (TpSvcDBusProperties *self, const gchar *name,
                  const GValue *value, GError **error)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;

    DEBUG ("called for %s", priv->unique_name);
    return (mcd_account_set_string_val (account, name, value, error)
            != SET_RESULT_ERROR);
}

static void
get_display_name (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);

    mcd_account_get_string_val (account, name, value);
}

static gboolean
set_icon (TpSvcDBusProperties *self, const gchar *name, const GValue *value,
          GError **error)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;

    DEBUG ("called for %s", priv->unique_name);
    return (mcd_account_set_string_val (account, name, value, error)
            != SET_RESULT_ERROR);
}

static void
get_icon (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);

    mcd_account_get_string_val (account, name, value);
}

static void
get_valid (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;

    g_value_init (value, G_TYPE_BOOLEAN);
    g_value_set_boolean (value, priv->valid);
}

static void
get_has_been_online (TpSvcDBusProperties *self, const gchar *name,
                     GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;

    g_value_init (value, G_TYPE_BOOLEAN);
    g_value_set_boolean (value, priv->has_been_online);
}

/**
 * mcd_account_set_enabled:
 * @account: the #McdAccount
 * @enabled: %TRUE if the account is to be enabled
 * @write_out: %TRUE if this should be written to the keyfile
 * @error: return location for an error condition
 *
 * Returns: %TRUE on success
 */
gboolean
_mcd_account_set_enabled (McdAccount *account,
                          gboolean enabled,
                          gboolean write_out,
                          GError **error)
{
    McdAccountPrivate *priv = account->priv;

    if (priv->always_on && !enabled)
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_PERMISSION_DENIED,
                     "Account %s cannot be disabled",
                     priv->unique_name);
        return FALSE;
    }

    if (priv->enabled != enabled)
    {
        GValue value = { 0, };
        const gchar *name = mcd_account_get_unique_name (account);

        if (!enabled)
            mcd_account_request_presence (account,
                                          TP_CONNECTION_PRESENCE_TYPE_OFFLINE,
                                          "offline", NULL);

        priv->enabled = enabled;

        g_value_init (&value, G_TYPE_BOOLEAN);
        g_value_set_boolean (&value, enabled);

        mcd_storage_set_value (priv->storage, name,
                               MC_ACCOUNTS_KEY_ENABLED, &value, FALSE);

        if (write_out)
            mcd_storage_commit (priv->storage, name);

        mcd_account_changed_property (account, "Enabled", &value);

        g_value_unset (&value);

        if (enabled)
        {
            mcd_account_request_presence_int (account,
                                              priv->req_presence_type,
                                              priv->req_presence_status,
                                              priv->req_presence_message);
            _mcd_account_maybe_autoconnect (account);
        }
    }

    return TRUE;
}

static gboolean
set_enabled (TpSvcDBusProperties *self, const gchar *name, const GValue *value,
             GError **error)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    gboolean enabled;

    DEBUG ("called for %s", priv->unique_name);

    if (!G_VALUE_HOLDS_BOOLEAN (value))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Expected boolean for Enabled, but got %s",
                     G_VALUE_TYPE_NAME (value));
        return FALSE;
    }

    enabled = g_value_get_boolean (value);

    return _mcd_account_set_enabled (account, enabled, TRUE, error);
}

static void
get_enabled (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;

    g_value_init (value, G_TYPE_BOOLEAN);
    g_value_set_boolean (value, priv->enabled);
}

static gboolean
set_service (TpSvcDBusProperties *self, const gchar *name,
             const GValue *value, GError **error)
{
    McdAccount *account = MCD_ACCOUNT (self);
    SetResult ret = SET_RESULT_ERROR;
    gboolean proceed = TRUE;
    static GRegex *rule = NULL;
    static gsize service_re_init = 0;

    if (g_once_init_enter (&service_re_init))
    {
        GError *regex_error = NULL;
        rule = g_regex_new ("^(?:[a-z][a-z0-9_-]*)?$",
                            G_REGEX_CASELESS|G_REGEX_DOLLAR_ENDONLY,
                            0, &regex_error);
        g_assert_no_error (regex_error);
        g_once_init_leave (&service_re_init, 1);
    }

    if (G_VALUE_HOLDS_STRING (value))
      proceed = g_regex_match (rule, g_value_get_string (value), 0, NULL);

    /* if value is not a string, mcd_account_set_string_val will set *
     * the appropriate error for us: don't duplicate that logic here */
    if (proceed)
    {
        ret = mcd_account_set_string_val (account, name, value, error);
    }
    else
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Invalid service '%s': Must consist of ASCII alphanumeric "
                     "characters, underscores (_) and hyphens (-) only, and "
                     "start with a letter",
                     g_value_get_string (value));
    }

    return (ret != SET_RESULT_ERROR);
}

static void
get_service (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);

    mcd_account_get_string_val (account, name, value);
}

static gboolean
set_nickname (TpSvcDBusProperties *self, const gchar *name,
              const GValue *value, GError **error)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    SetResult ret;

    DEBUG ("called for %s", priv->unique_name);
    ret = mcd_account_set_string_val (account, name, value, error);

    if (ret == SET_RESULT_CHANGED && priv->connection != NULL)
    {
        /* this is a no-op if the connection doesn't support it */
        _mcd_connection_set_nickname (priv->connection,
                                      g_value_get_string (value));
    }

    return (ret != SET_RESULT_ERROR);
}

static void
get_nickname (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);

    mcd_account_get_string_val (account, name, value);
}

static gboolean
set_avatar (TpSvcDBusProperties *self, const gchar *name, const GValue *value,
            GError **error)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    const gchar *mime_type;
    const GArray *avatar;
    GValueArray *va;

    DEBUG ("called for %s", priv->unique_name);

    if (!G_VALUE_HOLDS (value, TP_STRUCT_TYPE_AVATAR))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Unexpected type for Avatar: wanted (ay,s), got %s",
                     G_VALUE_TYPE_NAME (value));
        return FALSE;
    }

    va = g_value_get_boxed (value);
    avatar = g_value_get_boxed (va->values);
    mime_type = g_value_get_string (va->values + 1);

    if (!_mcd_account_set_avatar (account, avatar, mime_type, NULL, error))
    {
        return FALSE;
    }

    tp_svc_account_interface_avatar_emit_avatar_changed (account);
    return TRUE;
}

static void
get_avatar (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    gchar *mime_type;
    GArray *avatar = NULL;
    GType type = TP_STRUCT_TYPE_AVATAR;
    GValueArray *va;

    _mcd_account_get_avatar (account, &avatar, &mime_type);
    if (!avatar)
        avatar = g_array_new (FALSE, FALSE, 1);

    g_value_init (value, type);
    g_value_take_boxed (value, dbus_g_type_specialized_construct (type));
    va = (GValueArray *) g_value_get_boxed (value);
    g_value_take_boxed (va->values, avatar);
    g_value_take_string (va->values + 1, mime_type);
}

static void
get_parameters (TpSvcDBusProperties *self, const gchar *name,
                GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    GHashTable *params = _mcd_account_dup_parameters (account);

    g_value_init (value, TP_HASH_TYPE_STRING_VARIANT_MAP);
    g_value_take_boxed (value, params);
}

gboolean
_mcd_account_presence_type_is_settable (TpConnectionPresenceType type)
{
    switch (type)
    {
        case TP_CONNECTION_PRESENCE_TYPE_UNSET:
        case TP_CONNECTION_PRESENCE_TYPE_UNKNOWN:
        case TP_CONNECTION_PRESENCE_TYPE_ERROR:
            return FALSE;

        default:
            return TRUE;
    }
}

static gboolean
_presence_type_is_online (TpConnectionPresenceType type)
{
    switch (type)
    {
        case TP_CONNECTION_PRESENCE_TYPE_UNSET:
        case TP_CONNECTION_PRESENCE_TYPE_OFFLINE:
        case TP_CONNECTION_PRESENCE_TYPE_UNKNOWN:
        case TP_CONNECTION_PRESENCE_TYPE_ERROR:
            return FALSE;

        default:
            return TRUE;
    }
}

static gboolean
set_automatic_presence (TpSvcDBusProperties *self,
                        const gchar *name, const GValue *value, GError **error)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    const gchar *status, *message;
    TpConnectionPresenceType type;
    gboolean changed = FALSE;
    GValueArray *va;
    const gchar *account_name = mcd_account_get_unique_name (account);

    DEBUG ("called for %s", account_name);

    if (!G_VALUE_HOLDS (value, TP_STRUCT_TYPE_SIMPLE_PRESENCE))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Unexpected type for AutomaticPresence: wanted (u,s,s), "
                     "got %s", G_VALUE_TYPE_NAME (value));
        return FALSE;
    }

    va = g_value_get_boxed (value);
    type = g_value_get_uint (va->values);
    status = g_value_get_string (va->values + 1);
    message = g_value_get_string (va->values + 2);

    if (!_presence_type_is_online (type))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "AutomaticPresence must be an online presence, not %d",
                     type);
        return FALSE;
    }

    DEBUG ("setting automatic presence: %d, %s, %s", type, status, message);

    if (priv->auto_presence_type != type)
    {
        GValue presence = { 0 };

        g_value_init (&presence, G_TYPE_INT);
        g_value_set_int (&presence, type);

        mcd_storage_set_value (priv->storage, account_name,
                               MC_ACCOUNTS_KEY_AUTO_PRESENCE_TYPE,
                               &presence, FALSE);
        priv->auto_presence_type = type;
        changed = TRUE;
    }

    if (tp_strdiff (priv->auto_presence_status, status))
    {
        const gchar *new_status = NULL;

        if (status != NULL && status[0] != 0)
            new_status = status;

        mcd_storage_set_string (priv->storage, account_name,
                                MC_ACCOUNTS_KEY_AUTO_PRESENCE_STATUS,
                                new_status, FALSE);

        g_free (priv->auto_presence_status);
        priv->auto_presence_status = g_strdup (status);
        changed = TRUE;
    }

    if (tp_strdiff (priv->auto_presence_message, message))
    {
        const gchar *new_message = NULL;

        if (!tp_str_empty (message))
            new_message = message;

        mcd_storage_set_string (priv->storage, account_name,
                                MC_ACCOUNTS_KEY_AUTO_PRESENCE_MESSAGE,
                                new_message, FALSE);

        g_free (priv->auto_presence_message);
        priv->auto_presence_message = g_strdup (message);
        changed = TRUE;
    }


    if (changed)
    {
        mcd_storage_commit (priv->storage, account_name);
        mcd_account_changed_property (account, name, value);
    }

    return TRUE;
}

static void
get_automatic_presence (TpSvcDBusProperties *self,
			const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    gchar *presence, *message;
    gint presence_type;
    GType type;
    GValueArray *va;

    presence_type = priv->auto_presence_type;
    presence = priv->auto_presence_status;
    message = priv->auto_presence_message;

    type = TP_STRUCT_TYPE_SIMPLE_PRESENCE;
    g_value_init (value, type);
    g_value_take_boxed (value, dbus_g_type_specialized_construct (type));
    va = (GValueArray *) g_value_get_boxed (value);
    g_value_set_uint (va->values, presence_type);
    g_value_set_static_string (va->values + 1, presence);
    g_value_set_static_string (va->values + 2, message);
}

static gboolean
set_connect_automatically (TpSvcDBusProperties *self,
                           const gchar *name, const GValue *value,
                           GError **error)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    gboolean connect_automatically;

    DEBUG ("called for %s", priv->unique_name);

    if (!G_VALUE_HOLDS_BOOLEAN (value))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Expected boolean for ConnectAutomatically, but got %s",
                     G_VALUE_TYPE_NAME (value));
        return FALSE;
    }

    connect_automatically = g_value_get_boolean (value);

    if (priv->always_on && !connect_automatically)
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_PERMISSION_DENIED,
                     "Account %s always connects automatically",
                     priv->unique_name);
        return FALSE;
    }

    if (priv->connect_automatically != connect_automatically)
    {
        const gchar *account_name = mcd_account_get_unique_name (account);
        mcd_storage_set_value (priv->storage, account_name,
                               MC_ACCOUNTS_KEY_CONNECT_AUTOMATICALLY,
                               value, FALSE);

        priv->connect_automatically = connect_automatically;
        mcd_storage_commit (priv->storage, account_name);
        mcd_account_changed_property (account, name, value);

        if (connect_automatically)
        {
            _mcd_account_maybe_autoconnect (account);
        }
    }

    return TRUE;
}

static void
get_connect_automatically (TpSvcDBusProperties *self,
			   const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;

    DEBUG ("called for %s", priv->unique_name);
    g_value_init (value, G_TYPE_BOOLEAN);
    g_value_set_boolean (value, priv->connect_automatically);
}

static void
get_connection (TpSvcDBusProperties *self, const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    const gchar *object_path;

    g_value_init (value, DBUS_TYPE_G_OBJECT_PATH);
    if (priv->connection &&
	(object_path = mcd_connection_get_object_path (priv->connection)))
	g_value_set_boxed (value, object_path);
    else
	g_value_set_static_boxed (value, "/");
}

static void
get_connection_status (TpSvcDBusProperties *self,
		       const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);

    g_value_init (value, G_TYPE_UINT);
    g_value_set_uint (value, account->priv->conn_status);
}

static void
get_connection_status_reason (TpSvcDBusProperties *self,
			      const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);

    g_value_init (value, G_TYPE_UINT);
    g_value_set_uint (value, account->priv->conn_reason);
}

static void
get_connection_error (TpSvcDBusProperties *self,
                      const gchar *name,
                      GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);

    g_value_init (value, G_TYPE_STRING);
    g_value_set_string (value, account->priv->conn_dbus_error);
}

static void
get_connection_error_details (TpSvcDBusProperties *self,
                              const gchar *name,
                              GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);

    g_value_init (value, TP_HASH_TYPE_STRING_VARIANT_MAP);
    g_value_set_boxed (value, account->priv->conn_error_details);
}

static void
get_current_presence (TpSvcDBusProperties *self, const gchar *name,
		      GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    gchar *status, *message;
    gint presence_type;
    GType type;
    GValueArray *va;

    presence_type = priv->curr_presence_type;
    status = priv->curr_presence_status;
    message = priv->curr_presence_message;

    type = TP_STRUCT_TYPE_SIMPLE_PRESENCE;
    g_value_init (value, type);
    g_value_take_boxed (value, dbus_g_type_specialized_construct (type));
    va = (GValueArray *) g_value_get_boxed (value);
    g_value_set_uint (va->values, presence_type);
    g_value_set_static_string (va->values + 1, status);
    g_value_set_static_string (va->values + 2, message);
}

static gboolean
set_requested_presence (TpSvcDBusProperties *self,
                        const gchar *name, const GValue *value,
                        GError **error)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    const gchar *status, *message;
    gint type;
    GValueArray *va;

    DEBUG ("called for %s", priv->unique_name);

    if (!G_VALUE_HOLDS (value, TP_STRUCT_TYPE_SIMPLE_PRESENCE))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Unexpected type for RequestedPresence: wanted (u,s,s), "
                     "got %s", G_VALUE_TYPE_NAME (value));
        return FALSE;
    }

    va = g_value_get_boxed (value);
    type = (gint)g_value_get_uint (va->values);
    status = g_value_get_string (va->values + 1);
    message = g_value_get_string (va->values + 2);

    if (priv->always_on && !_presence_type_is_online (type))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_PERMISSION_DENIED,
                     "Account %s cannot be taken offline", priv->unique_name);
        return FALSE;
    }

    if (!_mcd_account_presence_type_is_settable (type))
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "RequestedPresence %d cannot be set on yourself", type);
        return FALSE;
    }

    DEBUG ("setting requested presence: %d, %s, %s", type, status, message);

    if (mcd_account_request_presence_int (account, type, status, message))
    {
	mcd_account_changed_property (account, name, value);
    }

    return TRUE;
}

static void
get_requested_presence (TpSvcDBusProperties *self,
			const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;
    gchar *presence, *message;
    gint presence_type;
    GType type;
    GValueArray *va;

    presence_type = priv->req_presence_type;
    presence = priv->req_presence_status;
    message = priv->req_presence_message;

    type = TP_STRUCT_TYPE_SIMPLE_PRESENCE;
    g_value_init (value, type);
    g_value_take_boxed (value, dbus_g_type_specialized_construct (type));
    va = (GValueArray *) g_value_get_boxed (value);
    g_value_set_uint (va->values, presence_type);
    g_value_set_static_string (va->values + 1, presence);
    g_value_set_static_string (va->values + 2, message);
}

static void
get_changing_presence (TpSvcDBusProperties *self,
                       const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;

    g_value_init (value, G_TYPE_BOOLEAN);
    g_value_set_boolean (value, priv->changing_presence);
}

static void
get_normalized_name (TpSvcDBusProperties *self,
		     const gchar *name, GValue *value)
{
    McdAccount *account = MCD_ACCOUNT (self);

    mcd_account_get_string_val (account, name, value);
}

static McpAccountStorage *
get_storage_plugin (McdAccount *account)
{
  McdAccountPrivate *priv = account->priv;
  const gchar *account_name = mcd_account_get_unique_name (account);

  if (priv->storage_plugin != NULL)
    return priv->storage_plugin;

  priv->storage_plugin = mcd_storage_get_plugin (priv->storage, account_name);

  if (priv->storage_plugin != NULL)
      g_object_ref (priv->storage_plugin);

   return priv->storage_plugin;
}

static void
get_storage_provider (TpSvcDBusProperties *self,
    const gchar *name, GValue *value)
{
  McdAccount *account = MCD_ACCOUNT (self);
  McpAccountStorage *storage_plugin = get_storage_plugin (account);

  g_value_init (value, G_TYPE_STRING);

  if (storage_plugin != NULL)
    g_value_set_string (value, mcp_account_storage_provider (storage_plugin));
  else
    g_value_set_static_string (value, "");
}

static void
get_storage_identifier (TpSvcDBusProperties *self,
    const gchar *name, GValue *value)
{

  McdAccount *account = MCD_ACCOUNT (self);
  McpAccountStorage *storage_plugin = get_storage_plugin (account);
  GValue identifier = { 0 };

  g_value_init (value, G_TYPE_VALUE);

  if (storage_plugin != NULL)
    {
      mcp_account_storage_get_identifier (
          storage_plugin, account->priv->unique_name, &identifier);
    }
  else
    {
      g_value_init (&identifier, G_TYPE_UINT);

      g_value_set_uint (&identifier, 0);
    }

  g_value_set_boxed (value, &identifier);

  g_value_unset (&identifier);
}

static void
get_storage_specific_info (TpSvcDBusProperties *self,
    const gchar *name, GValue *value)
{
  GHashTable *storage_specific_info;
  McdAccount *account = MCD_ACCOUNT (self);
  McpAccountStorage *storage_plugin = get_storage_plugin (account);

  g_value_init (value, TP_HASH_TYPE_STRING_VARIANT_MAP);

  if (storage_plugin != NULL)
    storage_specific_info = mcp_account_storage_get_additional_info (
        storage_plugin, account->priv->unique_name);
  else
    storage_specific_info = g_hash_table_new (g_str_hash, g_str_equal);

  g_value_take_boxed (value, storage_specific_info);
}

static void
get_storage_restrictions (TpSvcDBusProperties *self,
    const gchar *name, GValue *value)
{
  TpStorageRestrictionFlags flags;
  McdAccount *account = MCD_ACCOUNT (self);
  McpAccountStorage *storage_plugin = get_storage_plugin (account);

  g_value_init (value, G_TYPE_UINT);

  g_return_if_fail (storage_plugin != NULL);

  flags = mcp_account_storage_get_restrictions (storage_plugin,
      account->priv->unique_name);

  g_value_set_uint (value, flags);
}

static const McdDBusProp account_properties[] = {
    { "Interfaces", NULL, mcd_dbus_get_interfaces },
    { "DisplayName", set_display_name, get_display_name },
    { "Icon", set_icon, get_icon },
    { "Valid", NULL, get_valid },
    { "Enabled", set_enabled, get_enabled },
    { "Nickname", set_nickname, get_nickname },
    { "Service", set_service, get_service  },
    { "Parameters", NULL, get_parameters },
    { "AutomaticPresence", set_automatic_presence, get_automatic_presence },
    { "ConnectAutomatically", set_connect_automatically, get_connect_automatically },
    { "Connection", NULL, get_connection },
    { "ConnectionStatus", NULL, get_connection_status },
    { "ConnectionStatusReason", NULL, get_connection_status_reason },
    { "ConnectionError", NULL, get_connection_error },
    { "ConnectionErrorDetails", NULL, get_connection_error_details },
    { "CurrentPresence", NULL, get_current_presence },
    { "RequestedPresence", set_requested_presence, get_requested_presence },
    { "ChangingPresence", NULL, get_changing_presence },
    { "NormalizedName", NULL, get_normalized_name },
    { "HasBeenOnline", NULL, get_has_been_online },
    { 0 },
};

static const McdDBusProp account_avatar_properties[] = {
    { "Avatar", set_avatar, get_avatar },
    { 0 },
};

static const McdDBusProp account_storage_properties[] = {
    { "StorageProvider", NULL, get_storage_provider },
    { "StorageIdentifier", NULL, get_storage_identifier },
    { "StorageSpecificInformation", NULL, get_storage_specific_info },
    { "StorageRestrictions", NULL, get_storage_restrictions },
    { 0 },
};

static void
account_avatar_iface_init (TpSvcAccountInterfaceAvatarClass *iface,
			   gpointer iface_data)
{
}

static void
account_storage_iface_init (TpSvcAccountInterfaceStorageClass *iface,
                             gpointer iface_data)
{
}

static void
get_hidden (TpSvcDBusProperties *self,
    const gchar *name, GValue *value)
{
  g_value_init (value, G_TYPE_BOOLEAN);
  g_object_get_property (G_OBJECT (self), "hidden", value);
}

static gboolean
set_hidden (TpSvcDBusProperties *self,
    const gchar *name,
    const GValue *value,
    GError **error)
{
  McdAccount *account = MCD_ACCOUNT (self);
  McdAccountPrivate *priv = account->priv;
  const gchar *account_name = mcd_account_get_unique_name (account);

  if (!G_VALUE_HOLDS_BOOLEAN (value))
    {
      g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
          "Hidden must be set to a boolean, not a %s",
          G_VALUE_TYPE_NAME (value));
      return FALSE;
    }

  /* Technically this property is immutable after the account's been created,
   * but currently it's not easy for this code to tell whether or not this is
   * a create-time property. It would probably be better if the create-time
   * properties were passed into us as a construct-time GObject property. But
   * that's a job for another month.
   *
   * So for now we check whether the value has changed, and violate the spec
   * by making this property mutable (at least with the keyfile backend).
   */
  if (mcd_storage_set_value (priv->storage, account_name,
          MC_ACCOUNTS_KEY_HIDDEN, value, FALSE))
    {
      mcd_storage_commit (priv->storage, account_name);
      mcd_account_changed_property (account, MC_ACCOUNTS_KEY_HIDDEN, value);
      g_object_set_property (G_OBJECT (self), "hidden", value);
    }

  return TRUE;
}

static const McdDBusProp account_hidden_properties[] = {
    { "Hidden", set_hidden, get_hidden },
    { 0 },
};

static void
account_hidden_iface_init (
    McSvcAccountInterfaceHiddenClass *iface,
    gpointer iface_data)
{
  /* wow, it's pretty crap that I need this. */
}

static void
properties_iface_init (TpSvcDBusPropertiesClass *iface, gpointer iface_data)
{
#define IMPLEMENT(x) tp_svc_dbus_properties_implement_##x (\
    iface, dbusprop_##x)
    IMPLEMENT(set);
    IMPLEMENT(get);
    IMPLEMENT(get_all);
#undef IMPLEMENT
}

static GType
mc_param_type (const TpConnectionManagerParam *param)
{
    if (G_UNLIKELY (param == NULL)) return G_TYPE_INVALID;
    if (G_UNLIKELY (!param->dbus_signature)) return G_TYPE_INVALID;

    switch (param->dbus_signature[0])
    {
    case DBUS_TYPE_STRING:
	return G_TYPE_STRING;

    case DBUS_TYPE_BYTE:
        return G_TYPE_UCHAR;

    case DBUS_TYPE_INT16:
    case DBUS_TYPE_INT32:
	return G_TYPE_INT;

    case DBUS_TYPE_UINT16:
    case DBUS_TYPE_UINT32:
	return G_TYPE_UINT;

    case DBUS_TYPE_BOOLEAN:
	return G_TYPE_BOOLEAN;

    case DBUS_TYPE_DOUBLE:
        return G_TYPE_DOUBLE;

    case DBUS_TYPE_OBJECT_PATH:
        return DBUS_TYPE_G_OBJECT_PATH;

    case DBUS_TYPE_INT64:
        return G_TYPE_INT64;

    case DBUS_TYPE_UINT64:
        return G_TYPE_UINT64;

    case DBUS_TYPE_ARRAY:
        if (param->dbus_signature[1] == DBUS_TYPE_STRING)
            return G_TYPE_STRV;
        /* other array types are not supported:
         * fall through the default case */
    default:
        g_warning ("skipping parameter %s, unknown type %s",
                   param->name, param->dbus_signature);
    }
    return G_TYPE_INVALID;
}

typedef struct
{
    McdAccount *self;
    DBusGMethodInvocation *context;
} RemoveMethodData;

static void
account_remove_delete_cb (McdAccount *account, const GError *error,
                          gpointer user_data)
{
    RemoveMethodData *data = (RemoveMethodData *) user_data;

    if (error != NULL)
    {
        dbus_g_method_return_error (data->context, (GError *) error);
        return;
    }

    if (!data->self->priv->removed)
    {
        data->self->priv->removed = TRUE;
        tp_svc_account_emit_removed (data->self);
    }

    tp_svc_account_return_from_remove (data->context);

    g_slice_free (RemoveMethodData, data);
}

static void
account_remove (TpSvcAccount *svc, DBusGMethodInvocation *context)
{
    McdAccount *self = MCD_ACCOUNT (svc);
    RemoveMethodData *data;

    data = g_slice_new0 (RemoveMethodData);
    data->self = self;
    data->context = context;

    DEBUG ("called");
    mcd_account_delete (self, account_remove_delete_cb, data);
}

/* tell the account that one of its properties has changed behind its back:  *
 * (as opposed to an external change triggered by DBus, for example) - This  *
 * typically occurs because an internal component (such as a storage plugin) *
 * wishes to notify us that something has changed.
 * This will trigger an update when the callback receives the new value     */
void
mcd_account_property_changed (McdAccount *account, const gchar *name)
{
    /* parameters are handled en bloc, but first make sure it's a valid name */
    if (g_str_has_prefix (name, "param-"))
    {
        const gchar *param = name + strlen ("param-");
        GValue value = { 0, };

        /* check to see if the parameter was/is a valid one. If it was real,
         * kick off the en-bloc parameters update signal
         */
        if (mcd_account_get_parameter (account, param, &value, NULL))
            mcd_account_property_changed (account, "Parameters");
        else
            DEBUG ("Unknown/unset parameter %s", name);
    }
    else
    {
        guint i = 0;
        const McdDBusProp *prop = NULL;

        /* find the property update handler */
        for (; prop == NULL && account_properties[i].name != NULL; i++)
        {
            if (g_str_equal (name, account_properties[i].name))
                prop = &account_properties[i];
        }

        /* is a known property: invoke the getter method for it (if any): *
         * then issue the change notification (DBus signals etc) for it   */
        if (prop != NULL)
        {
            TpSvcDBusProperties *self = TP_SVC_DBUS_PROPERTIES (account);

            if (prop->getprop != NULL)
            {
                GValue value = { 0 };

                prop->getprop (self, name, &value);
                mcd_account_changed_property (account, prop->name, &value);
                g_value_unset (&value);
            }
            else
            {
                DEBUG ("Valid DBus property %s with no get method was changed"
                       " - cannot notify change since we cannot get its value",
                      name);
            }
        }
    }
}


static void
mcd_account_check_parameters (McdAccount *account,
                              CheckParametersCb callback,
                              gpointer user_data)
{
    McdAccountPrivate *priv = account->priv;
    TpConnectionManagerProtocol *protocol;
    const TpConnectionManagerParam *param;

    g_return_if_fail (callback != NULL);

    DEBUG ("called for %s", priv->unique_name);
    protocol = _mcd_manager_dup_protocol (priv->manager, priv->protocol_name);

    if (protocol == NULL)
    {
        DEBUG ("CM %s doesn't implement protocol %s", priv->manager_name,
            priv->protocol_name);
        callback (account, FALSE, user_data);
        return;
    }

    for (param = protocol->params; param->name != NULL; param++)
    {
        if (!(param->flags & TP_CONN_MGR_PARAM_FLAG_REQUIRED))
            continue;

        if (!mcd_account_get_parameter (account, param->name, NULL, NULL))
        {
            DEBUG ("missing required parameter %s", param->name);
            callback (account, FALSE, user_data);
            goto out;
        }
    }

    callback (account, TRUE, user_data);
out:
    tp_connection_manager_protocol_free (protocol);
}

static void
set_parameters_maybe_autoconnect_cb (McdAccount *account,
                                     gboolean valid,
                                     gpointer user_data G_GNUC_UNUSED)
{
    /* Strictly speaking this doesn't need to be called unless valid is TRUE,
     * but calling it in all cases gives us clearer debug output */
    _mcd_account_maybe_autoconnect (account);
}

static void
set_parameters_finish (McdAccount *account,
                       GHashTable *params,
                       GQueue *dbus_properties)
{
    McdAccountPrivate *priv = account->priv;

    if (mcd_account_get_connection_status (account) ==
        TP_CONNECTION_STATUS_CONNECTED)
    {
        const gchar *name;
        const GValue *value;

        /* This is a bit sketchy; we modify the list the caller gave us. */
        while ((name = g_queue_pop_head (dbus_properties)) != NULL)
        {
            DEBUG ("updating parameter %s", name);
            value = g_hash_table_lookup (params, name);
            _mcd_connection_update_property (priv->connection, name, value);
        }
    }

    mcd_account_check_validity (account,
                                set_parameters_maybe_autoconnect_cb, NULL);
}

static void
set_parameters_unset_check_present (McdAccount *account,
                                    GPtrArray *not_yet,
                                    const gchar *name)
{
    if (mcd_account_get_parameter (account, name, NULL, NULL))
    {
        DEBUG ("unsetting %s", name);
        /* pessimistically assume that removing any parameter merits
         * reconnection (in a perfect implementation, if the
         * Has_Default flag was set we'd check whether the current
         * value is the default already) */

        g_ptr_array_add (not_yet, g_strdup (name));
    }
}

static void
set_parameter_changed (GQueue *dbus_properties,
                       GPtrArray *not_yet,
                       const TpConnectionManagerParam *param)
{
    DEBUG ("Parameter %s changed", param->name);

    /* can the param be updated on the fly? If yes, prepare to do so; and if
     * not, prepare to reset the connection */
    if (param->flags & TP_CONN_MGR_PARAM_FLAG_DBUS_PROPERTY)
    {
        g_queue_push_tail (dbus_properties, param->name);
    }
    else
    {
        g_ptr_array_add (not_yet, g_strdup (param->name));
    }
}

static gboolean
check_one_parameter (McdAccount *account,
                     TpConnectionManagerProtocol *protocol,
                     GQueue *dbus_properties,
                     GPtrArray *not_yet,
                     const gchar *name,
                     const GValue *new_value,
                     GError **error)
{
    const TpConnectionManagerParam *param =
        tp_connection_manager_protocol_get_param (protocol, name);
    GType type;

    if (param == NULL)
    {
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Protocol '%s' does not have parameter '%s'",
                     protocol->name, name);
        return FALSE;
    }

    type = mc_param_type (param);

    if (G_VALUE_TYPE (new_value) != type)
    {
        /* FIXME: define proper error */
        g_set_error (error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "parameter %s must be of type %s, not %s",
                     param->name,
                     g_type_name (type), G_VALUE_TYPE_NAME (new_value));
        return FALSE;
    }

    if (mcd_account_get_connection_status (account) ==
        TP_CONNECTION_STATUS_CONNECTED)
    {
        GValue current_value = { 0, };

        if (mcd_account_get_parameter (account, param->name, &current_value,
                                       NULL))
        {
            if (!value_is_same (&current_value, new_value))
                set_parameter_changed (dbus_properties, not_yet, param);

            g_value_unset (&current_value);
        }
        else
        {
            /* If it had no previous value, it's certainly changed. */
            set_parameter_changed (dbus_properties, not_yet, param);
        }
    }

    return TRUE;
}

static gboolean
check_parameters (McdAccount *account,
                  TpConnectionManagerProtocol *protocol,
                  GHashTable *params,
                  const gchar **unset,
                  GQueue *dbus_properties,
                  GPtrArray *not_yet,
                  GError **error)
{
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init (&iter, params);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        if (!check_one_parameter (account, protocol, dbus_properties, not_yet,
                                  key, value, error))
            return FALSE;
    }

    return TRUE;
}

/*
 * _mcd_account_set_parameters:
 * @account: the #McdAccount.
 * @name: the parameter name.
 * @params: names and values of parameters to set
 * @unset: names of parameters to unset
 * @callback: function to be called when finished
 * @user_data: data to be passed to @callback
 *
 * Alter the account parameters.
 *
 */
void
_mcd_account_set_parameters (McdAccount *account, GHashTable *params,
                             const gchar **unset,
                             McdAccountSetParametersCb callback,
                             gpointer user_data)
{
    McdAccountPrivate *priv = account->priv;
    GQueue dbus_properties;
    GPtrArray *not_yet;
    GError *error = NULL;
    guint unset_size;
    TpConnectionManagerProtocol *protocol = NULL;
    GHashTableIter iter;
    gpointer key, value;
    const gchar **unset_iter;

    DEBUG ("called");
    if (G_UNLIKELY (!priv->manager && !load_manager (account)))
    {
        g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Manager %s not found", priv->manager_name);
        goto error;
    }

    protocol = _mcd_manager_dup_protocol (priv->manager, priv->protocol_name);

    if (G_UNLIKELY (protocol == NULL))
    {
        g_set_error (&error, TP_ERRORS, TP_ERROR_INVALID_ARGUMENT,
                     "Protocol %s not found", priv->protocol_name);
        goto error;
    }

    unset_size = (unset != NULL) ? g_strv_length ((gchar **) unset) : 0;

    g_queue_init (&dbus_properties);
    /* pessimistically assume that every parameter mentioned will be deferred
     * until reconnection */
    not_yet = g_ptr_array_sized_new (g_hash_table_size (params) + unset_size);

    if (!check_parameters (account, protocol, params, unset, &dbus_properties,
                           not_yet, &error))
    {
        g_queue_clear (&dbus_properties);
        /* FIXME: we leak not_yet, like we did before. This is fixed in a
         * subsequent commit. */
        goto error;
    }

    /* If we made it here, all the parameters to be set look kosher. We haven't
     * checked those that are meant to be unset. So now we actually commit the
     * updates, first setting new values, then clearing those in unset.
     */
    g_hash_table_iter_init (&iter, params);
    while (g_hash_table_iter_next (&iter, &key, &value))
    {
        _mcd_account_set_parameter (account, key, value);
    }

    for (unset_iter = unset;
         unset_iter != NULL && *unset_iter != NULL;
         unset_iter++)
    {
        set_parameters_unset_check_present (account, not_yet, *unset_iter);
        _mcd_account_set_parameter (account, *unset_iter, NULL);
    }

    set_parameters_finish (account, params, &dbus_properties);

    if (callback != NULL)
    {
        callback (account, not_yet, NULL, user_data);
    }

    g_queue_clear (&dbus_properties);
    /* FIXME: not_yet is freed by the callback!!! */
    tp_connection_manager_protocol_free (protocol);
    return;

error:
    if (callback != NULL)
        callback (account, NULL, error, user_data);

    g_error_free (error);
    tp_clear_pointer (&protocol, tp_connection_manager_protocol_free);
}

static void
account_update_parameters_cb (McdAccount *account, GPtrArray *not_yet,
                              const GError *error, gpointer user_data)
{
    McdAccountPrivate *priv = account->priv;
    DBusGMethodInvocation *context = (DBusGMethodInvocation *) user_data;
    const gchar *account_name = mcd_account_get_unique_name (account);
    GHashTable *params;
    GValue value = { 0 };

    if (error != NULL)
    {
        dbus_g_method_return_error (context, (GError *) error);
        return;
    }

    /* Emit the PropertiesChanged signal */
    params = _mcd_account_dup_parameters (account);
    g_return_if_fail (params != NULL);

    g_value_init (&value, TP_HASH_TYPE_STRING_VARIANT_MAP);
    g_value_take_boxed (&value, params);
    mcd_account_changed_property (account, "Parameters", &value);
    g_value_unset (&value);

    /* Commit the changes to disk */
    mcd_storage_commit (priv->storage, account_name);

    /* And finally, return from UpdateParameters() */
    g_ptr_array_add (not_yet, NULL);

    tp_svc_account_return_from_update_parameters (context,
        (const gchar **) not_yet->pdata);

    g_ptr_array_foreach (not_yet, (GFunc) g_free, NULL);
    g_ptr_array_free (not_yet, TRUE);
}

static void
account_update_parameters (TpSvcAccount *self, GHashTable *set,
			   const gchar **unset, DBusGMethodInvocation *context)
{
    McdAccount *account = MCD_ACCOUNT (self);
    McdAccountPrivate *priv = account->priv;

    DEBUG ("called for %s", priv->unique_name);

    _mcd_account_set_parameters (account, set, unset,
                                 account_update_parameters_cb, context);
}

static void
account_reconnect (TpSvcAccount *service,
                   DBusGMethodInvocation *context)
{
    McdAccount *self = MCD_ACCOUNT (service);
    McdAccountPrivate *priv = self->priv;

    DEBUG ("%s", mcd_account_get_unique_name (self));

    /* if we can't, or don't want to, connect this method is a no-op */
    if (!priv->enabled ||
        !priv->valid ||
        priv->req_presence_type == TP_CONNECTION_PRESENCE_TYPE_OFFLINE)
    {
        DEBUG ("doing nothing (enabled=%c, valid=%c and "
               "combined presence=%i)",
               self->priv->enabled ? 'T' : 'F',
               self->priv->valid ? 'T' : 'F',
               self->priv->req_presence_type);
        tp_svc_account_return_from_reconnect (context);
        return;
    }

    /* FIXME: this isn't quite right. If we've just called RequestConnection
     * (possibly with out of date parameters) but we haven't got a Connection
     * back from the CM yet, the old parameters will still be used, I think
     * (I can't quite make out what actually happens). */
    if (priv->connection)
        mcd_connection_close (priv->connection);
    _mcd_account_connection_begin (self);

    /* FIXME: we shouldn't really return from this method until the
     * reconnection has actually happened, but that would require less tangled
     * integration between Account and Connection */
    tp_svc_account_return_from_reconnect (context);
}

static void
account_iface_init (TpSvcAccountClass *iface, gpointer iface_data)
{
#define IMPLEMENT(x) tp_svc_account_implement_##x (\
    iface, account_##x)
    IMPLEMENT(remove);
    IMPLEMENT(update_parameters);
    IMPLEMENT(reconnect);
#undef IMPLEMENT
}

static void
register_dbus_service (McdAccount *self,
                       const GError *error,
                       gpointer unused G_GNUC_UNUSED)
{
    DBusGConnection *dbus_connection;
    TpDBusDaemon *dbus_daemon;

    if (error != NULL)
    {
        /* due to some tangled error handling, the McdAccount might already
         * have been freed by the time we get here, so it's no longer safe to
         * dereference self here! */
        DEBUG ("%p failed to load: %s code %d: %s", self,
               g_quark_to_string (error->domain), error->code, error->message);
        return;
    }

    g_assert (MCD_IS_ACCOUNT (self));
    /* these are invariants - the storage is set at construct-time
     * and the object path is set in mcd_account_setup, both of which are
     * run before this callback can possibly be invoked */
    g_assert (self->priv->storage != NULL);
    g_assert (self->priv->object_path != NULL);

    dbus_daemon = self->priv->dbus_daemon;
    g_return_if_fail (dbus_daemon != NULL);

    dbus_connection = TP_PROXY (dbus_daemon)->dbus_connection;

    if (G_LIKELY (dbus_connection))
	dbus_g_connection_register_g_object (dbus_connection,
					     self->priv->object_path,
					     (GObject *) self);
}

static gboolean
mcd_account_setup (McdAccount *account)
{
    McdAccountPrivate *priv = account->priv;
    McdStorage *storage = priv->storage;
    const gchar *name = mcd_account_get_unique_name (account);

    priv->manager_name =
      mcd_storage_dup_string (storage, name, MC_ACCOUNTS_KEY_MANAGER);

    if (priv->manager_name == NULL)
    {
        g_warning ("Account '%s' has no manager", name);
        goto broken_account;
    }

    priv->protocol_name =
      mcd_storage_dup_string (storage, name, MC_ACCOUNTS_KEY_PROTOCOL);

    if (priv->protocol_name == NULL)
    {
        g_warning ("Account has no protocol");
        goto broken_account;
    }

    priv->object_path = g_strconcat (TP_ACCOUNT_OBJECT_PATH_BASE, name, NULL);

    if (!priv->always_on)
    {
        priv->enabled =
          mcd_storage_get_boolean (storage, name, MC_ACCOUNTS_KEY_ENABLED);

        priv->connect_automatically =
          mcd_storage_get_boolean (storage, name,
                                   MC_ACCOUNTS_KEY_CONNECT_AUTOMATICALLY);
    }

    priv->has_been_online =
      mcd_storage_get_boolean (storage, name, MC_ACCOUNTS_KEY_HAS_BEEN_ONLINE);
    priv->hidden =
      mcd_storage_get_boolean (storage, name, MC_ACCOUNTS_KEY_HIDDEN);

    /* load the automatic presence */
    priv->auto_presence_type =
      mcd_storage_get_integer (storage, name,
                               MC_ACCOUNTS_KEY_AUTO_PRESENCE_TYPE);

    /* If invalid or something, force it to AVAILABLE - we want the auto
     * presence type to be an online status */
    if (!_presence_type_is_online (priv->auto_presence_type))
    {
        priv->auto_presence_type = TP_CONNECTION_PRESENCE_TYPE_AVAILABLE;
        g_free (priv->auto_presence_status);
        priv->auto_presence_status = g_strdup ("available");
    }
    else
    {
        g_free (priv->auto_presence_status);
        priv->auto_presence_status =
          mcd_storage_dup_string (storage, name,
                                  MC_ACCOUNTS_KEY_AUTO_PRESENCE_STATUS);
    }

    g_free (priv->auto_presence_message);
    priv->auto_presence_message =
      mcd_storage_dup_string (storage, name,
                              MC_ACCOUNTS_KEY_AUTO_PRESENCE_MESSAGE);

    /* check the manager */
    if (!priv->manager && !load_manager (account))
    {
	g_warning ("Could not find manager `%s'", priv->manager_name);
        mcd_account_loaded (account);
    }

    /* even though the manager is absent or unusable, we still register *
     * the accounts dbus name as it is otherwise acceptably configured  */

    _mcd_account_load (account, register_dbus_service, NULL);
    return TRUE;

broken_account:
    /* normally, various callbacks would release locks when the manager      *
     * became ready: however, this cannot happen for an incomplete account   *
     * as it never gets a manager: We therefore invoke the account callbacks *
     * right now so the account manager doesn't hang around forever waiting  *
     * for an event that cannot happen (at least until the account is fixed) */
    mcd_account_loaded (account);
    return FALSE;
}

static void
set_property (GObject *obj, guint prop_id,
	      const GValue *val, GParamSpec *pspec)
{
    McdAccount *account = MCD_ACCOUNT (obj);
    McdAccountPrivate *priv = account->priv;

    switch (prop_id)
    {
    case PROP_STORAGE:
        g_assert (priv->storage == NULL);
        priv->storage = g_value_dup_object (val);
	break;

      case PROP_DBUS_DAEMON:
        g_assert (priv->dbus_daemon == NULL);
        priv->dbus_daemon = g_value_dup_object (val);
        break;

    case PROP_NAME:
	g_assert (priv->unique_name == NULL);
	priv->unique_name = g_value_dup_string (val);
	break;

    case PROP_ALWAYS_ON:
        priv->always_on = g_value_get_boolean (val);

        if (priv->always_on)
        {
            priv->enabled = TRUE;
            priv->connect_automatically = TRUE;
            priv->req_presence_type = priv->auto_presence_type;
            priv->req_presence_status = g_strdup (priv->auto_presence_status);
            priv->req_presence_message = g_strdup (priv->auto_presence_message);
        }

        break;
    case PROP_HIDDEN:
        priv->hidden = g_value_get_boolean (val);
        break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
	break;
    }
}

static void
get_property (GObject *obj, guint prop_id,
	      GValue *val, GParamSpec *pspec)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (obj);

    switch (prop_id)
    {
    case PROP_DBUS_DAEMON:
        g_value_set_object (val, priv->dbus_daemon);
	break;
    case PROP_NAME:
	g_value_set_string (val, priv->unique_name);
	break;
    case PROP_HIDDEN:
        g_value_set_boolean (val, priv->hidden);
        break;
    default:
	G_OBJECT_WARN_INVALID_PROPERTY_ID (obj, prop_id, pspec);
	break;
    }
}

static void
_mcd_account_finalize (GObject *object)
{
    McdAccount *account = MCD_ACCOUNT (object);
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);

    DEBUG ("%p (%s)", object, priv->unique_name);

    if (priv->changed_properties)
	g_hash_table_destroy (priv->changed_properties);
    if (priv->properties_source != 0)
	g_source_remove (priv->properties_source);

    tp_clear_pointer (&priv->curr_presence_status, g_free);
    tp_clear_pointer (&priv->curr_presence_message, g_free);

    tp_clear_pointer (&priv->req_presence_status, g_free);
    tp_clear_pointer (&priv->req_presence_message, g_free);

    tp_clear_pointer (&priv->auto_presence_status, g_free);
    tp_clear_pointer (&priv->auto_presence_message, g_free);

    tp_clear_pointer (&priv->manager_name, g_free);
    tp_clear_pointer (&priv->protocol_name, g_free);
    tp_clear_pointer (&priv->unique_name, g_free);
    tp_clear_pointer (&priv->object_path, g_free);

    G_OBJECT_CLASS (mcd_account_parent_class)->finalize (object);
}

static void
_mcd_account_dispose (GObject *object)
{
    McdAccount *self = MCD_ACCOUNT (object);
    McdAccountPrivate *priv = self->priv;

    DEBUG ("%p (%s)", object, priv->unique_name);

    if (!self->priv->removed)
    {
        self->priv->removed = TRUE;
        tp_svc_account_emit_removed (self);
    }

    if (priv->online_requests)
    {
        GError *error;
        GList *list = priv->online_requests;

        error = g_error_new (TP_ERRORS, TP_ERROR_DISCONNECTED,
                             "Disposing account %s", priv->unique_name);
        while (list)
        {
            McdOnlineRequestData *data = list->data;

            data->callback (MCD_ACCOUNT (object), data->user_data, error);
            g_slice_free (McdOnlineRequestData, data);
            list = g_list_delete_link (list, list);
        }
        g_error_free (error);
	priv->online_requests = NULL;
    }

    tp_clear_object (&priv->manager);
    tp_clear_object (&priv->storage_plugin);
    tp_clear_object (&priv->storage);
    tp_clear_object (&priv->dbus_daemon);

    _mcd_account_set_connection_context (self, NULL);
    _mcd_account_set_connection (self, NULL);

    G_OBJECT_CLASS (mcd_account_parent_class)->dispose (object);
}

static GObject *
_mcd_account_constructor (GType type, guint n_params,
                          GObjectConstructParam *params)
{
    GObjectClass *object_class = (GObjectClass *)mcd_account_parent_class;
    McdAccount *account;
    McdAccountPrivate *priv;

    account = MCD_ACCOUNT (object_class->constructor (type, n_params, params));
    priv = account->priv;

    g_return_val_if_fail (account != NULL, NULL);

    if (G_UNLIKELY (!priv->storage || !priv->unique_name))
    {
        g_object_unref (account);
        return NULL;
    }

    return (GObject *) account;
}

static void
_mcd_account_constructed (GObject *object)
{
    GObjectClass *object_class = (GObjectClass *)mcd_account_parent_class;
    McdAccount *account = MCD_ACCOUNT (object);

    if (object_class->constructed)
        object_class->constructed (object);

    DEBUG ("%p (%s)", object, account->priv->unique_name);

    mcd_account_setup (account);
}

static void
mcd_account_class_init (McdAccountClass * klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS (klass);
    g_type_class_add_private (object_class, sizeof (McdAccountPrivate));

    object_class->constructor = _mcd_account_constructor;
    object_class->constructed = _mcd_account_constructed;
    object_class->dispose = _mcd_account_dispose;
    object_class->finalize = _mcd_account_finalize;
    object_class->set_property = set_property;
    object_class->get_property = get_property;

    klass->check_request = _mcd_account_check_request_real;

    g_object_class_install_property
        (object_class, PROP_DBUS_DAEMON,
         g_param_spec_object ("dbus-daemon", "DBus daemon", "DBus daemon",
                              TP_TYPE_DBUS_DAEMON,
                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property
        (object_class, PROP_STORAGE,
         g_param_spec_object ("storage", "storage",
                               "storage", MCD_TYPE_STORAGE,
                               G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property
        (object_class, PROP_NAME,
         g_param_spec_string ("name", "Unique name", "Unique name",
                              NULL,
                              G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY));

    g_object_class_install_property
        (object_class, PROP_ALWAYS_ON,
         g_param_spec_boolean ("always-on", "Always on?", "Always on?",
                              FALSE,
                              G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY |
                              G_PARAM_STATIC_STRINGS));

    g_object_class_install_property
        (object_class, PROP_HIDDEN,
         g_param_spec_boolean ("hidden", "Hidden?", "Is this account hidden?",
                               FALSE,
                               G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));
    /* Signals */
    _mcd_account_signals[CONNECTION_STATUS_CHANGED] =
	g_signal_new ("connection-status-changed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      0,
		      NULL, NULL, _mcd_marshal_VOID__UINT_UINT,
		      G_TYPE_NONE,
		      2, G_TYPE_UINT, G_TYPE_UINT);
    _mcd_account_signals[VALIDITY_CHANGED] =
	g_signal_new ("validity-changed",
		      G_OBJECT_CLASS_TYPE (klass),
		      G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
		      0,
		      NULL, NULL, g_cclosure_marshal_VOID__BOOLEAN,
		      G_TYPE_NONE, 1,
		      G_TYPE_BOOLEAN);

    _mcd_account_compat_class_init (klass);
    _mcd_account_connection_class_init (klass);

    account_ready_quark = g_quark_from_static_string ("mcd_account_load");
}

static void
mcd_account_init (McdAccount *account)
{
    McdAccountPrivate *priv;

    priv = G_TYPE_INSTANCE_GET_PRIVATE ((account),
					MCD_TYPE_ACCOUNT,
					McdAccountPrivate);
    account->priv = priv;

    priv->req_presence_type = TP_CONNECTION_PRESENCE_TYPE_OFFLINE;
    priv->req_presence_status = g_strdup ("offline");
    priv->req_presence_message = g_strdup ("");

    priv->curr_presence_type = TP_CONNECTION_PRESENCE_TYPE_OFFLINE;
    priv->curr_presence_status = g_strdup ("offline");
    priv->curr_presence_status = g_strdup ("");

    priv->always_on = FALSE;
    priv->enabled = FALSE;
    priv->connect_automatically = FALSE;

    priv->changing_presence = FALSE;

    priv->auto_presence_type = TP_CONNECTION_PRESENCE_TYPE_AVAILABLE;
    priv->auto_presence_status = g_strdup ("available");
    priv->auto_presence_message = g_strdup ("");

    /* initializes the interfaces */
    mcd_dbus_init_interfaces_instances (account);

    priv->conn_status = TP_CONNECTION_STATUS_DISCONNECTED;
    priv->conn_reason = TP_CONNECTION_STATUS_REASON_REQUESTED;
    priv->conn_dbus_error = g_strdup ("");
    priv->conn_error_details = g_hash_table_new_full (g_str_hash, g_str_equal,
        g_free, (GDestroyNotify) tp_g_value_slice_free);

    priv->changed_properties = g_hash_table_new_full (g_str_hash, g_str_equal,
        NULL, (GDestroyNotify) tp_g_value_slice_free);
}

McdAccount *
mcd_account_new (McdAccountManager *account_manager, const gchar *name)
{
    gpointer *obj;
    McdStorage *storage = mcd_account_manager_get_storage (account_manager);
    TpDBusDaemon *dbus = mcd_account_manager_get_dbus_daemon (account_manager);

    obj = g_object_new (MCD_TYPE_ACCOUNT,
                        "storage", storage,
                        "dbus-daemon", dbus,
			"name", name,
			NULL);
    return MCD_ACCOUNT (obj);
}

McdStorage *
_mcd_account_get_storage (McdAccount *account)
{
    return account->priv->storage;
}

TpDBusDaemon *
mcd_account_get_dbus_daemon (McdAccount *account)
{
    return account->priv->dbus_daemon;
}


/*
 * mcd_account_is_valid:
 * @account: the #McdAccount.
 *
 * Checks that the account is usable:
 * - Manager, protocol and TODO presets (if specified) must exist
 * - All required parameters for the protocol must be set
 *
 * Returns: %TRUE if the account is valid, false otherwise.
 */
gboolean
mcd_account_is_valid (McdAccount *account)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    return priv->valid;
}

/**
 * mcd_account_is_enabled:
 * @account: the #McdAccount.
 *
 * Checks if the account is enabled:
 *
 * Returns: %TRUE if the account is enabled, false otherwise.
 */
gboolean
mcd_account_is_enabled (McdAccount *account)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    return priv->enabled;
}

gboolean
_mcd_account_is_hidden (McdAccount *account)
{
    g_return_val_if_fail (MCD_IS_ACCOUNT (account), FALSE);

    return account->priv->hidden;
}

const gchar *
mcd_account_get_unique_name (McdAccount *account)
{
    return account->priv->unique_name;
}

const gchar *
mcd_account_get_object_path (McdAccount *account)
{
    return account->priv->object_path;
}

/**
 * _mcd_account_dup_parameters:
 * @account: the #McdAccount.
 *
 * Get the parameters set for this account. The resulting #GHashTable will be
 * newly allocated and must be g_hash_table_unref()'d after use.
 *
 * Returns: @account's current parameters, or %NULL if they could not be
 *          retrieved.
 */
GHashTable *
_mcd_account_dup_parameters (McdAccount *account)
{
    McdAccountPrivate *priv;
    TpConnectionManagerProtocol *protocol;
    const TpConnectionManagerParam *param;
    GHashTable *params;

    g_return_val_if_fail (MCD_IS_ACCOUNT (account), NULL);

    priv = account->priv;

    DEBUG ("called");
    if (!priv->manager && !load_manager (account))
    {
        DEBUG ("unable to load manager for account %s", priv->unique_name);
        return NULL;
    }

    protocol = _mcd_manager_dup_protocol (priv->manager,
                                          priv->protocol_name);

    if (G_UNLIKELY (protocol == NULL))
    {
        DEBUG ("unable to get protocol for %s account %s", priv->protocol_name,
               priv->unique_name);
        return NULL;
    }

    params = g_hash_table_new_full (g_str_hash, g_str_equal,
                                    g_free,
                                    (GDestroyNotify) tp_g_value_slice_free);

    for (param = protocol->params; param->name != NULL; param++)
    {
        GValue v = { 0, };

        if (mcd_account_get_parameter (account, param->name, &v, NULL))
        {
            g_hash_table_insert (params, g_strdup (param->name),
                                 tp_g_value_slice_dup (&v));
            g_value_unset (&v);
        }
    }

    tp_connection_manager_protocol_free (protocol);
    return params;
}

/**
 * mcd_account_request_presence:
 * @account: the #McdAccount.
 * @presence: a #TpConnectionPresenceType.
 * @status: presence status.
 * @message: presence status message.
 *
 * Request a presence status on the account.
 */
void
mcd_account_request_presence (McdAccount *account,
			      TpConnectionPresenceType presence,
			      const gchar *status, const gchar *message)
{
    if (mcd_account_request_presence_int (account, presence, status, message))
    {
	GValue value = { 0 };
	GType type;
        GValueArray *va;

	type = TP_STRUCT_TYPE_SIMPLE_PRESENCE;
	g_value_init (&value, type);
	g_value_take_boxed (&value, dbus_g_type_specialized_construct (type));
	va = (GValueArray *) g_value_get_boxed (&value);
	g_value_set_uint (va->values, presence);
	g_value_set_static_string (va->values + 1, status);
	g_value_set_static_string (va->values + 2, message);
	mcd_account_changed_property (account, "RequestedPresence", &value);
	g_value_unset (&value);
    }
}

static void
mcd_account_update_self_presence (McdAccount *account,
                                  TpConnectionPresenceType presence,
                                  const gchar *status,
                                  const gchar *message)
{
    McdAccountPrivate *priv = account->priv;
    gboolean changed = FALSE;
    GValue value = { 0 };

    if (priv->curr_presence_type != presence)
    {
	priv->curr_presence_type = presence;
	changed = TRUE;
    }
    if (tp_strdiff (priv->curr_presence_status, status))
    {
	g_free (priv->curr_presence_status);
	priv->curr_presence_status = g_strdup (status);
	changed = TRUE;
    }
    if (tp_strdiff (priv->curr_presence_message, message))
    {
	g_free (priv->curr_presence_message);
	priv->curr_presence_message = g_strdup (message);
	changed = TRUE;
    }

    if (_mcd_connection_presence_info_is_ready (priv->connection))
    {
        _mcd_account_set_changing_presence (account, FALSE);
    }

    if (!changed) return;

    g_value_init (&value, TP_STRUCT_TYPE_SIMPLE_PRESENCE);
    g_value_take_boxed (&value,
                        tp_value_array_build (3,
                                              G_TYPE_UINT, presence,
                                              G_TYPE_STRING, status,
                                              G_TYPE_STRING, message,
                                              G_TYPE_INVALID));
    mcd_account_changed_property (account, "CurrentPresence", &value);
    g_value_unset (&value);
}


static void
on_conn_self_presence_changed (McdConnection *connection,
                               TpConnectionPresenceType presence,
                               const gchar *status,
                               const gchar *message,
                               gpointer user_data)
{
    McdAccount *account = MCD_ACCOUNT (user_data);
    McdAccountPrivate *priv = account->priv;

    g_assert (priv->connection == connection);
    mcd_account_update_self_presence (account, presence, status, message);
}

/* TODO: remove when the relative members will become public */
void
mcd_account_get_requested_presence (McdAccount *account,
				    TpConnectionPresenceType *presence,
				    const gchar **status,
				    const gchar **message)
{
    McdAccountPrivate *priv = account->priv;

    if (presence != NULL)
        *presence = priv->req_presence_type;

    if (status != NULL)
        *status = priv->req_presence_status;

    if (message != NULL)
        *message = priv->req_presence_message;
}

void
_mcd_account_get_requested_presence (McdAccount *account,
                                     TpConnectionPresenceType *presence,
                                     const gchar **status,
                                     const gchar **message)
{
    McdAccountPrivate *priv = account->priv;

    if (presence != NULL)
        *presence = priv->req_presence_type;

    if (status != NULL)
        *status = priv->req_presence_status;

    if (message != NULL)
        *message = priv->req_presence_message;
}

/* TODO: remove when the relative members will become public */
void
mcd_account_get_current_presence (McdAccount *account,
				  TpConnectionPresenceType *presence,
				  const gchar **status,
				  const gchar **message)
{
    McdAccountPrivate *priv = account->priv;

    if (presence != NULL)
        *presence = priv->curr_presence_type;

    if (status != NULL)
        *status = priv->curr_presence_status;

    if (message != NULL)
        *message = priv->curr_presence_message;
}

gboolean
mcd_account_get_connect_automatically (McdAccount *account)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    return priv->connect_automatically;
}

/* TODO: remove when the relative members will become public */
void
mcd_account_get_automatic_presence (McdAccount *account,
				    TpConnectionPresenceType *presence,
				    const gchar **status,
				    const gchar **message)
{
    McdAccountPrivate *priv = account->priv;

    if (presence != NULL)
        *presence = priv->auto_presence_type;

    if (status != NULL)
        *status = priv->auto_presence_status;

    if (message != NULL)
        *message = priv->auto_presence_message;
}

/* TODO: remove when the relative members will become public */
const gchar *
mcd_account_get_manager_name (McdAccount *account)
{
    McdAccountPrivate *priv = account->priv;

    return priv->manager_name;
}

/* TODO: remove when the relative members will become public */
const gchar *
mcd_account_get_protocol_name (McdAccount *account)
{
    McdAccountPrivate *priv = account->priv;

    return priv->protocol_name;
}

/**
 * mcd_account_get_cm:
 * @account: an account
 *
 * Fetches the connection manager through which @account connects. If @account
 * is not ready, or is invalid (perhaps because the connection manager is
 * missing), this may be %NULL.
 *
 * Returns: the connection manager through which @account connects, or %NULL.
 */
TpConnectionManager *
mcd_account_get_cm (McdAccount *account)
{
    g_return_val_if_fail (account != NULL, NULL);
    g_return_val_if_fail (MCD_IS_ACCOUNT (account), NULL);

    return mcd_manager_get_tp_proxy (account->priv->manager);
}

void
_mcd_account_set_normalized_name (McdAccount *account, const gchar *name)
{
    McdAccountPrivate *priv = account->priv;
    GValue value = { 0, };
    const gchar *account_name = mcd_account_get_unique_name (account);

    DEBUG ("called (%s)", name);

    g_value_init (&value, G_TYPE_STRING);
    g_value_set_static_string (&value, name);

    mcd_storage_set_value (priv->storage,
                           account_name,
                           MC_ACCOUNTS_KEY_NORMALIZED_NAME,
                           &value, FALSE);
    mcd_storage_commit (priv->storage, account_name);
    mcd_account_changed_property (account, MC_ACCOUNTS_KEY_NORMALIZED_NAME,
                                  &value);

    g_value_unset (&value);
}

gchar *
mcd_account_get_normalized_name (McdAccount *account)
{
    McdAccountPrivate *priv = account->priv;
    const gchar *account_name = mcd_account_get_unique_name (account);

    return mcd_storage_dup_string (priv->storage,
                                   account_name,
                                   MC_ACCOUNTS_KEY_NORMALIZED_NAME);
}

void
_mcd_account_set_avatar_token (McdAccount *account, const gchar *token)
{
    McdAccountPrivate *priv = account->priv;
    const gchar *account_name = mcd_account_get_unique_name (account);

    DEBUG ("called (%s)", token);
    mcd_storage_set_string (priv->storage,
                            account_name,
                            MC_ACCOUNTS_KEY_AVATAR_TOKEN,
                            token, FALSE);

    mcd_storage_commit (priv->storage, account_name);
}

gchar *
_mcd_account_get_avatar_token (McdAccount *account)
{
    McdAccountPrivate *priv = account->priv;
    const gchar *account_name = mcd_account_get_unique_name (account);

    return mcd_storage_dup_string (priv->storage,
                                   account_name,
                                   MC_ACCOUNTS_KEY_AVATAR_TOKEN);
}

gboolean
_mcd_account_set_avatar (McdAccount *account, const GArray *avatar,
			const gchar *mime_type, const gchar *token,
			GError **error)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    const gchar *account_name = mcd_account_get_unique_name (account);
    gchar *data_dir, *filename;

    DEBUG ("called");
    data_dir = get_account_data_path (priv);
    filename = g_build_filename (data_dir, MC_AVATAR_FILENAME, NULL);
    if (!g_file_test (data_dir, G_FILE_TEST_EXISTS))
	g_mkdir_with_parents (data_dir, 0700);
    _mcd_chmod_private (data_dir);
    g_free (data_dir);

    if (G_LIKELY(avatar) && avatar->len > 0)
    {
	if (!g_file_set_contents (filename, avatar->data,
				  (gssize)avatar->len, error))
	{
	    g_warning ("%s: writing to file %s failed", G_STRLOC,
		       filename);
	    g_free (filename);
	    return FALSE;
	}
    }
    else
    {
	g_remove (filename);
    }
    g_free (filename);

    if (mime_type != NULL)
        mcd_storage_set_string (priv->storage,
                                account_name,
                                MC_ACCOUNTS_KEY_AVATAR_MIME,
                                mime_type, FALSE);

    if (token)
    {
        gchar *prev_token;

        prev_token = _mcd_account_get_avatar_token (account);

        mcd_storage_set_string (priv->storage,
                                account_name,
                                MC_ACCOUNTS_KEY_AVATAR_TOKEN,
                                token, FALSE);

        if (!prev_token || strcmp (prev_token, token) != 0)
            tp_svc_account_interface_avatar_emit_avatar_changed (account);

        g_free (prev_token);
    }
    else
    {
        mcd_storage_set_value (priv->storage,
                               account_name,
                               MC_ACCOUNTS_KEY_AVATAR_TOKEN,
                               NULL, FALSE);

        /* this is a no-op if the connection doesn't support avatars */
        if (priv->connection != NULL)
        {
            _mcd_connection_set_avatar (priv->connection, avatar, mime_type);
        }
    }

    mcd_storage_commit (priv->storage, account_name);

    return TRUE;
}

void
_mcd_account_get_avatar (McdAccount *account, GArray **avatar,
                         gchar **mime_type)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    const gchar *account_name = mcd_account_get_unique_name (account);
    gchar *filename;

    if (mime_type != NULL)
        *mime_type =  mcd_storage_dup_string (priv->storage, account_name,
                                              MC_ACCOUNTS_KEY_AVATAR_MIME);

    if (avatar == NULL)
        return;

    *avatar = NULL;

    filename = _mcd_account_get_avatar_filename (account);

    if (filename && g_file_test (filename, G_FILE_TEST_EXISTS))
    {
	GError *error = NULL;
	gchar *data = NULL;
	gsize length;
	if (g_file_get_contents (filename, &data, &length, &error))
	{
	    if (length > 0 && length < G_MAXUINT) 
	    {
		*avatar = g_array_new (FALSE, FALSE, 1);
		(*avatar)->data = data;
		(*avatar)->len = (guint)length;
	    }
	}
	else
	{
            DEBUG ("error reading %s: %s", filename, error->message);
	    g_error_free (error);
	}
    }
    g_free (filename);
}

static void
mcd_account_connection_self_nickname_changed_cb (McdAccount *account,
                                                 const gchar *alias,
                                                 McdConnection *connection)
{
    GValue value = { 0 };

    g_value_init (&value, G_TYPE_STRING);
    g_value_set_static_string (&value, alias);
    mcd_account_set_string_val (account, MC_ACCOUNTS_KEY_ALIAS, &value, NULL);
    g_value_unset (&value);
}

gchar *
mcd_account_get_alias (McdAccount *account)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    const gchar *account_name = mcd_account_get_unique_name (account);

    return mcd_storage_dup_string (priv->storage, account_name,
                                   MC_ACCOUNTS_KEY_ALIAS);
}

void
_mcd_account_online_request_completed (McdAccount *account, GError *error)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    GList *list;

    list = priv->online_requests;

    while (list)
    {
        McdOnlineRequestData *data = list->data;

        data->callback (account, data->user_data, error);
        g_slice_free (McdOnlineRequestData, data);
        list = g_list_delete_link (list, list);
    }
    if (error)
        g_error_free (error);
    priv->online_requests = NULL;
}

GList *
_mcd_account_get_online_requests (McdAccount *account)
{
    g_return_val_if_fail (MCD_IS_ACCOUNT (account), NULL);

    return account->priv->online_requests;
}

static inline void
process_online_requests (McdAccount *account,
			 TpConnectionStatus status,
			 TpConnectionStatusReason reason)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    GError *error;

    switch (status)
    {
    case TP_CONNECTION_STATUS_CONNECTED:
        error = NULL;
	break;
    case TP_CONNECTION_STATUS_DISCONNECTED:
        error = g_error_new (TP_ERRORS, TP_ERROR_DISCONNECTED,
                             "Account %s disconnected with reason %d",
                             priv->unique_name, reason);
	break;
    default:
	return;
    }
    _mcd_account_online_request_completed (account, error);
}

static void
on_conn_status_changed (McdConnection *connection,
                        TpConnectionStatus status,
                        TpConnectionStatusReason reason,
                        TpConnection *tp_conn,
                        McdAccount *account)
{
    const gchar *dbus_error = NULL;
    const GHashTable *details = NULL;

    if (tp_conn != NULL)
    {
        dbus_error = tp_connection_get_detailed_error (tp_conn, &details);
    }

    _mcd_account_set_connection_status (account, status, reason, tp_conn,
                                        dbus_error, details);
}

/* clear the "register" flag, if necessary */
static void
clear_register (McdAccount *self)
{
    GHashTable *params = _mcd_account_dup_parameters (self);

    if (params == NULL)
    {
        DEBUG ("no params returned");
        return;
    }

    if (tp_asv_get_boolean (params, "register", NULL))
    {
        GValue value = { 0 };
        const gchar *account_name = mcd_account_get_unique_name (self);

        _mcd_account_set_parameter (self, "register", NULL);

        g_hash_table_remove (params, "register");

        g_value_init (&value, TP_HASH_TYPE_STRING_VARIANT_MAP);
        g_value_take_boxed (&value, params);
        mcd_account_changed_property (self, "Parameters", &value);
        g_value_unset (&value);

        mcd_storage_commit (self->priv->storage, account_name);
    }
    else
    {
      g_hash_table_unref (params);
    }
}

void
_mcd_account_set_connection_status (McdAccount *account,
                                    TpConnectionStatus status,
                                    TpConnectionStatusReason reason,
                                    TpConnection *tp_conn,
                                    const gchar *dbus_error,
                                    const GHashTable *details)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    gboolean changed = FALSE;

    DEBUG ("%s: %u because %u", priv->unique_name, status, reason);

    mcd_account_freeze_properties (account);

    if (status == TP_CONNECTION_STATUS_CONNECTED)
    {
        _mcd_account_set_has_been_online (account);
        clear_register (account);

        DEBUG ("clearing connection error details");
        g_free (priv->conn_dbus_error);
        priv->conn_dbus_error = g_strdup ("");
        g_hash_table_remove_all (priv->conn_error_details);

    }
    else if (status == TP_CONNECTION_STATUS_DISCONNECTED)
    {
        if (dbus_error == NULL)
            dbus_error = "";

        if (tp_strdiff (dbus_error, priv->conn_dbus_error))
        {
            DEBUG ("changing detailed D-Bus error from '%s' to '%s'",
                   priv->conn_dbus_error, dbus_error);
            g_free (priv->conn_dbus_error);
            priv->conn_dbus_error = g_strdup (dbus_error);
            changed = TRUE;
        }

        /* to avoid having to do deep comparisons, we assume that any change to
         * or from a non-empty hash table is interesting. */
        if ((details != NULL && tp_asv_size (details) > 0) ||
            tp_asv_size (priv->conn_error_details) > 0)
        {
            DEBUG ("changing error details");
            g_hash_table_remove_all (priv->conn_error_details);

            if (details != NULL)
                tp_g_hash_table_update (priv->conn_error_details,
                                        (GHashTable *) details,
                                        (GBoxedCopyFunc) g_strdup,
                                        (GBoxedCopyFunc) tp_g_value_slice_dup);

            changed = TRUE;
        }
    }

    if (priv->tp_connection != tp_conn
        || (tp_conn != NULL && status == TP_CONNECTION_STATUS_DISCONNECTED))
    {
        tp_clear_object (&priv->tp_connection);

        if (tp_conn != NULL && status != TP_CONNECTION_STATUS_DISCONNECTED)
            priv->tp_connection = g_object_ref (tp_conn);
        else
            priv->tp_connection = NULL;

        changed = TRUE;
    }

    if (status != priv->conn_status)
    {
        DEBUG ("changing connection status from %u to %u", priv->conn_status,
               status);
	priv->conn_status = status;
	changed = TRUE;
    }

    if (reason != priv->conn_reason)
    {
        DEBUG ("changing connection status reason from %u to %u",
               priv->conn_reason, reason);
	priv->conn_reason = reason;
	changed = TRUE;
    }

    if (changed)
    {
        GValue value = { 0 };

        _mcd_account_tp_connection_changed (account, priv->tp_connection);

        g_value_init (&value, G_TYPE_UINT);
        g_value_set_uint (&value, priv->conn_status);
        mcd_account_changed_property (account, "ConnectionStatus", &value);
        g_value_set_uint (&value, priv->conn_reason);
        mcd_account_changed_property (account, "ConnectionStatusReason",
                                      &value);
        g_value_unset (&value);

        g_value_init (&value, G_TYPE_STRING);
        g_value_set_string (&value, priv->conn_dbus_error);
        mcd_account_changed_property (account, "ConnectionError", &value);
        g_value_unset (&value);

        g_value_init (&value, TP_HASH_TYPE_STRING_VARIANT_MAP);
        g_value_set_boxed (&value, priv->conn_error_details);
        mcd_account_changed_property (account, "ConnectionErrorDetails",
                                      &value);
        g_value_unset (&value);
    }

    mcd_account_thaw_properties (account);

    process_online_requests (account, status, reason);

    if (changed)
	g_signal_emit (account,
		       _mcd_account_signals[CONNECTION_STATUS_CHANGED], 0,
		       status, reason);
}

TpConnectionStatus
mcd_account_get_connection_status (McdAccount *account)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    return priv->conn_status;
}

TpConnectionStatusReason
mcd_account_get_connection_status_reason (McdAccount *account)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    return priv->conn_reason;
}

void
_mcd_account_tp_connection_changed (McdAccount *account,
                                    TpConnection *tp_conn)
{
    GValue value = { 0 };

    g_value_init (&value, DBUS_TYPE_G_OBJECT_PATH);

    if (tp_conn == NULL)
    {
        g_value_set_static_boxed (&value, "/");
    }
    else
    {
        g_value_set_boxed (&value, tp_proxy_get_object_path (tp_conn));
    }

    mcd_account_changed_property (account, "Connection", &value);
    g_value_unset (&value);

    _mcd_storage_store_connections (account->priv->storage);
}

McdConnection *
mcd_account_get_connection (McdAccount *account)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    return priv->connection;
}

typedef struct
{
    McdAccountCheckValidityCb callback;
    gpointer user_data;
} CheckValidityData;

static void
check_validity_check_parameters_cb (McdAccount *account,
                                    gboolean valid,
                                    gpointer user_data)
{
    CheckValidityData *data = (CheckValidityData *) user_data;
    McdAccountPrivate *priv = account->priv;

    if (valid != priv->valid)
    {
        GValue value = { 0 };
        DEBUG ("Account validity changed (old: %d, new: %d)",
               priv->valid, valid);
        priv->valid = valid;
        g_signal_emit (account, _mcd_account_signals[VALIDITY_CHANGED], 0,
                       valid);
        g_value_init (&value, G_TYPE_BOOLEAN);
        g_value_set_boolean (&value, valid);
        mcd_account_changed_property (account, "Valid", &value);

        if (valid)
        {
            /* newly valid - try setting requested presence again */
            mcd_account_request_presence_int (account,
                                              priv->req_presence_type,
                                              priv->req_presence_status,
                                              priv->req_presence_message);
        }
    }

    if (data->callback != NULL)
        data->callback (account, valid, data->user_data);

    g_slice_free (CheckValidityData, data);
}

void
mcd_account_check_validity (McdAccount *account,
                            McdAccountCheckValidityCb callback,
                            gpointer user_data)
{
    CheckValidityData *data;

    g_return_if_fail (MCD_IS_ACCOUNT (account));

    data = g_slice_new0 (CheckValidityData);
    data->callback = callback;
    data->user_data = user_data;

    mcd_account_check_parameters (account, check_validity_check_parameters_cb,
                                  data);
}

/*
 * _mcd_account_connect_with_auto_presence:
 * @account: the #McdAccount.
 *
 * Request the account to go online with the configured AutomaticPresence.
 * This is appropriate in these situations:
 * - going online automatically because we've gained connectivity
 * - going online automatically in order to request a channel
 */
void
_mcd_account_connect_with_auto_presence (McdAccount *account)
{
    McdAccountPrivate *priv = account->priv;

    mcd_account_request_presence (account,
                                  priv->auto_presence_type,
                                  priv->auto_presence_status,
                                  priv->auto_presence_message);
}

/*
 * _mcd_account_online_request:
 * @account: the #McdAccount.
 * @callback: a #McdOnlineRequestCb.
 * @userdata: user data to be passed to @callback.
 *
 * If the account is online, call @callback immediately; else, try to put the
 * account online (set its presence to the automatic presence) and eventually
 * invoke @callback.
 *
 * @callback is always invoked exactly once.
 */
void
_mcd_account_online_request (McdAccount *account,
                             McdOnlineRequestCb callback,
                             gpointer userdata)
{
    McdAccountPrivate *priv = account->priv;
    McdOnlineRequestData *data;

    DEBUG ("connection status for %s is %d",
           priv->unique_name, priv->conn_status);
    if (priv->conn_status == TP_CONNECTION_STATUS_CONNECTED)
    {
        /* invoke the callback now */
        DEBUG ("%s is already connected", priv->unique_name);
        callback (account, userdata, NULL);
        return;
    }

    if (priv->loaded && !priv->valid)
    {
        /* FIXME: pick a better error and put it in telepathy-spec? */
        GError e = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
            "account isn't Valid (not enough information to put it online)" };

        DEBUG ("%s: %s", priv->unique_name, e.message);
        callback (account, userdata, &e);
        return;
    }

    if (priv->loaded && !priv->enabled)
    {
        /* FIXME: pick a better error and put it in telepathy-spec? */
        GError e = { TP_ERRORS, TP_ERROR_NOT_AVAILABLE,
            "account isn't Enabled" };

        DEBUG ("%s: %s", priv->unique_name, e.message);
        callback (account, userdata, &e);
        return;
    }

    /* listen to the StatusChanged signal */
    if (priv->loaded && priv->conn_status == TP_CONNECTION_STATUS_DISCONNECTED)
        _mcd_account_connect_with_auto_presence (account);

    /* now the connection should be in connecting state; insert the
     * callback in the online_requests hash table, which will be processed
     * in the connection-status-changed callback */
    data = g_slice_new (McdOnlineRequestData);
    data->callback = callback;
    data->user_data = userdata;
    priv->online_requests = g_list_append (priv->online_requests, data);
}

GKeyFile *
_mcd_account_get_keyfile (McdAccount *account)
{
    McdAccountPrivate *priv = MCD_ACCOUNT_PRIV (account);
    return priv->keyfile;
}

/* this is public because of mcd-account-compat */
gchar *
_mcd_account_get_avatar_filename (McdAccount *account)
{
    McdAccountPrivate *priv = account->priv;
    gchar *data_dir, *filename;

    data_dir = get_account_data_path (priv);
    DEBUG("data dir: %s", data_dir);
    filename = g_build_filename (data_dir, MC_AVATAR_FILENAME, NULL);
    g_free (data_dir);
    return filename;
}

static void
mcd_account_self_handle_inspected_cb (TpConnection *connection,
                                      const gchar **names,
                                      const GError *error,
                                      gpointer user_data,
                                      GObject *weak_object)
{
    McdAccount *self = MCD_ACCOUNT (weak_object);

    if (error)
    {
        g_warning ("%s: InspectHandles failed: %s", G_STRFUNC, error->message);
        return;
    }

    if (names != NULL && names[0] != NULL)
    {
        _mcd_account_set_normalized_name (self, names[0]);
    }
}

static void
mcd_account_connection_ready_cb (McdAccount *account,
                                 McdConnection *connection)
{
    McdAccountPrivate *priv = account->priv;
    gchar *nickname;
    TpConnection *tp_connection;
    GArray *self_handle_array;
    guint self_handle;
    TpConnectionStatus status;
    TpConnectionStatusReason reason;
    const gchar *dbus_error = NULL;
    const GHashTable *details = NULL;

    g_return_if_fail (MCD_IS_ACCOUNT (account));
    g_return_if_fail (connection == priv->connection);

    tp_connection = mcd_connection_get_tp_connection (connection);
    g_return_if_fail (tp_connection != NULL);
    g_return_if_fail (priv->tp_connection == NULL ||
                      tp_connection == priv->tp_connection);

    status = tp_connection_get_status (tp_connection, &reason);
    dbus_error = tp_connection_get_detailed_error (tp_connection, &details);
    _mcd_account_set_connection_status (account, status, reason,
                                        tp_connection, dbus_error, details);

    self_handle_array = g_array_sized_new (FALSE, FALSE, sizeof (guint), 1);
    self_handle = tp_connection_get_self_handle (tp_connection);
    g_array_append_val (self_handle_array, self_handle);
    tp_cli_connection_call_inspect_handles (tp_connection, -1,
                                            TP_HANDLE_TYPE_CONTACT,
                                            self_handle_array,
                                            mcd_account_self_handle_inspected_cb,
                                            NULL, NULL,
                                            (GObject *) account);
    g_array_free (self_handle_array, TRUE);

    /* FIXME: ideally, on protocols with server-stored nicknames, this should
     * only be done if the local Nickname has been changed since last time we
     * were online; Aliasing doesn't currently offer a way to tell whether
     * this is such a protocol, though. */

    nickname = mcd_account_get_alias (account);

    if (nickname != NULL)
    {
        /* this is a no-op if the connection doesn't support it */
        _mcd_connection_set_nickname (connection, nickname);
    }

    g_free (nickname);

    if (!tp_proxy_has_interface_by_id (tp_connection,
            TP_IFACE_QUARK_CONNECTION_INTERFACE_SIMPLE_PRESENCE))
    {
        /* This connection doesn't have SimplePresence, but it's online.
         * TpConnection only emits connection-ready when the account is online
         * and we've introspected it, so we know that if this interface isn't
         * present now, it's not going to appear.
         *
         * So, the spec says that we should set CurrentPresence to Unset.
         */
        mcd_account_update_self_presence (account,
            TP_CONNECTION_PRESENCE_TYPE_UNSET, "", "");
    }

}

void
_mcd_account_set_connection (McdAccount *account, McdConnection *connection)
{
    McdAccountPrivate *priv;

    g_return_if_fail (MCD_IS_ACCOUNT (account));
    priv = account->priv;
    if (connection == priv->connection) return;

    if (priv->connection)
    {
        g_signal_handlers_disconnect_by_func (priv->connection,
                                              on_connection_abort, account);
        g_signal_handlers_disconnect_by_func (priv->connection,
                                              on_conn_self_presence_changed,
                                              account);
        g_signal_handlers_disconnect_by_func (priv->connection,
                                              on_conn_status_changed,
                                              account);
        g_signal_handlers_disconnect_by_func (priv->connection,
                                              mcd_account_connection_ready_cb,
                                              account);
        g_object_unref (priv->connection);
    }

    tp_clear_object (&priv->tp_connection);

    priv->connection = connection;

    if (connection)
    {
        g_return_if_fail (MCD_IS_CONNECTION (connection));
        g_object_ref (connection);

        if (_mcd_connection_is_ready (connection))
        {
            mcd_account_connection_ready_cb (account, connection);
        }
        else
        {
            g_signal_connect_swapped (connection, "ready",
                G_CALLBACK (mcd_account_connection_ready_cb), account);
        }

        g_signal_connect_swapped (connection, "self-nickname-changed",
                G_CALLBACK (mcd_account_connection_self_nickname_changed_cb),
                account);

        g_signal_connect (connection, "self-presence-changed",
                          G_CALLBACK (on_conn_self_presence_changed), account);
        g_signal_connect (connection, "connection-status-changed",
                          G_CALLBACK (on_conn_status_changed), account);
        g_signal_connect (connection, "abort",
                          G_CALLBACK (on_connection_abort), account);
    }
    else
    {
        priv->conn_status = TP_CONNECTION_STATUS_DISCONNECTED;
        priv->transport = NULL;
    }
}

void
_mcd_account_set_has_been_online (McdAccount *account)
{
    if (!account->priv->has_been_online)
    {
        GValue value = { 0 };
        const gchar *account_name = mcd_account_get_unique_name (account);

        g_value_init (&value, G_TYPE_BOOLEAN);
        g_value_set_boolean (&value, TRUE);

        mcd_storage_set_value (account->priv->storage,
                               account_name,
                               MC_ACCOUNTS_KEY_HAS_BEEN_ONLINE,
                               &value, FALSE);
        account->priv->has_been_online = TRUE;
        mcd_storage_commit (account->priv->storage, account_name);
        mcd_account_changed_property (account, MC_ACCOUNTS_KEY_HAS_BEEN_ONLINE,
                                      &value);
        g_value_unset (&value);
    }
}

void
_mcd_account_request_temporary_presence (McdAccount *self,
                                         TpConnectionPresenceType type,
                                         const gchar *status)
{
    if (self->priv->connection != NULL)
    {
        _mcd_account_set_changing_presence (self, TRUE);

        _mcd_connection_request_presence (self->priv->connection,
                                          type, status, "");
    }
}

/**
 * mcd_account_connection_bind_transport:
 * @account: the #McdAccount.
 * @transport: the #McdTransport.
 *
 * Set @account as dependent on @transport; connectivity plugins should call
 * this function in the callback they registered with
 * mcd_plugin_register_account_connection(). This tells the account manager to
 * disconnect @account when @transport goes away.
 */
void
mcd_account_connection_bind_transport (McdAccount *account,
                                       McdTransport *transport)
{
    g_return_if_fail (MCD_IS_ACCOUNT (account));

    if (transport == account->priv->transport)
    {
        DEBUG ("account %s transport remains %p",
               account->priv->unique_name, transport);
    }
    else if (transport == NULL)
    {
        DEBUG ("unbinding account %s from transport %p",
               account->priv->unique_name, account->priv->transport);
        account->priv->transport = NULL;
    }
    else if (account->priv->transport == NULL)
    {
        DEBUG ("binding account %s to transport %p",
               account->priv->unique_name, transport);

        account->priv->transport = transport;
    }
    else
    {
        DEBUG ("disallowing migration of account %s from transport %p to %p",
               account->priv->unique_name, account->priv->transport,
               transport);
    }
}

McdTransport *
_mcd_account_connection_get_transport (McdAccount *account)
{
    g_return_val_if_fail (MCD_IS_ACCOUNT (account), NULL);

    return account->priv->transport;
}

McdAccountConnectionContext *
_mcd_account_get_connection_context (McdAccount *self)
{
    g_return_val_if_fail (MCD_IS_ACCOUNT (self), NULL);

    return self->priv->connection_context;
}

void
_mcd_account_set_connection_context (McdAccount *self,
                                     McdAccountConnectionContext *c)
{
    g_return_if_fail (MCD_IS_ACCOUNT (self));

    if (self->priv->connection_context != NULL)
    {
        _mcd_account_connection_context_free (self->priv->connection_context);
    }

    self->priv->connection_context = c;
}

gboolean
_mcd_account_get_always_on (McdAccount *self)
{
    g_return_val_if_fail (MCD_IS_ACCOUNT (self), FALSE);

    return self->priv->always_on;
}

gboolean
mcd_account_parameter_is_secret (McdAccount *self, const gchar *name)
{
    McdAccountPrivate *priv = self->priv;
    const TpConnectionManagerParam *param;

    param = mcd_manager_get_protocol_param (priv->manager,
                                            priv->protocol_name, name);

    return (param != NULL &&
        (param->flags & TP_CONN_MGR_PARAM_FLAG_SECRET) != 0);
}

void
_mcd_account_set_changing_presence (McdAccount *self, gboolean value)
{
    McdAccountPrivate *priv = self->priv;
    GValue changing_presence = { 0 };

    priv->changing_presence = value;

    g_value_init (&changing_presence, G_TYPE_BOOLEAN);
    g_value_set_boolean (&changing_presence, value);

    mcd_account_changed_property (self, "ChangingPresence",
                                  &changing_presence);

    g_value_unset (&changing_presence);
}
