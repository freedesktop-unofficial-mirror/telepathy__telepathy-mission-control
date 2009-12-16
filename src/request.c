/* A Telepathy ChannelRequest object
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

#include "request.h"

#include <dbus/dbus-glib.h>

#include "mcd-debug.h"

enum {
    PROP_0,
    PROP_ACCOUNT,
    PROP_ACCOUNT_PATH,
    PROP_USER_ACTION_TIME
};

struct _McdRequest {
    GObject parent;

    McdAccount *account;
    gint64 user_action_time;
};

struct _McdRequestClass {
    GObjectClass parent;
};

G_DEFINE_TYPE_WITH_CODE (McdRequest, _mcd_request, G_TYPE_OBJECT,
    /* no interfaces yet: */ (void) 0)

static void
_mcd_request_init (McdRequest *self)
{
  DEBUG ("%p", self);
}

static void
_mcd_request_constructed (GObject *object)
{
  McdRequest *self = (McdRequest *) object;
  void (*constructed) (GObject *) =
    G_OBJECT_CLASS (_mcd_request_parent_class)->constructed;

  if (constructed != NULL)
    constructed (object);

  g_return_if_fail (self->account != NULL);
}

static void
_mcd_request_get_property (GObject *object,
    guint prop_id,
    GValue *value,
    GParamSpec *pspec)
{
  McdRequest *self = (McdRequest *) object;

  switch (prop_id)
    {
    case PROP_ACCOUNT:
      g_value_set_object (value, self->account);
      break;

    case PROP_ACCOUNT_PATH:
      g_value_set_boxed (value, mcd_account_get_object_path (self->account));
      break;

    case PROP_USER_ACTION_TIME:
      g_value_set_int64 (value, self->user_action_time);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
_mcd_request_set_property (GObject *object,
    guint prop_id,
    const GValue *value,
    GParamSpec *pspec)
{
  McdRequest *self = (McdRequest *) object;

  switch (prop_id)
    {
    case PROP_ACCOUNT:
      g_assert (self->account == NULL); /* construct-only */
      self->account = g_value_dup_object (value);
      break;

    case PROP_USER_ACTION_TIME:
      g_assert (self->user_action_time == 0); /* construct-only */
      self->user_action_time = g_value_get_int64 (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
    }
}

static void
_mcd_request_dispose (GObject *object)
{
  McdRequest *self = (McdRequest *) object;
  GObjectFinalizeFunc dispose =
    G_OBJECT_CLASS (_mcd_request_parent_class)->dispose;

  DEBUG ("%p", object);

  if (self->account != NULL)
    {
      g_object_unref (self->account);
      self->account = NULL;
    }

  if (dispose != NULL)
    dispose (object);
}

static void
_mcd_request_class_init (
    McdRequestClass *cls)
{
  GObjectClass *object_class = (GObjectClass *) cls;

  object_class->constructed = _mcd_request_constructed;
  object_class->get_property = _mcd_request_get_property;
  object_class->set_property = _mcd_request_set_property;
  object_class->dispose = _mcd_request_dispose;

  g_object_class_install_property (object_class, PROP_ACCOUNT,
      g_param_spec_object ("account", "Account",
          "The underlying McdAccount",
          MCD_TYPE_ACCOUNT,
          G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY |
          G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_ACCOUNT_PATH,
      g_param_spec_boxed ("account-path", "Account path",
          "The object path of McdRequest:account",
          DBUS_TYPE_G_OBJECT_PATH,
          G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_USER_ACTION_TIME,
       g_param_spec_int64 ("user-action-time", "UserActionTime",
         "Time of user action in seconds since 1970",
         G_MININT64, G_MAXINT64, 0,
         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));
}

McdRequest *
_mcd_request_new (McdAccount *account,
    gint64 user_action_time)
{
  McdRequest *self;

  self = g_object_new (MCD_TYPE_REQUEST,
      "account", account,
      "user-action-time", user_action_time,
      NULL);
  DEBUG ("%p (for %p)", self, account);

  return self;
}

McdAccount *
_mcd_request_get_account (McdRequest *self)
{
  return self->account;
}

gint64
_mcd_request_get_user_action_time (McdRequest *self)
{
  return self->user_action_time;
}
