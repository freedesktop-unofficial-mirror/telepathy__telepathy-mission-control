# Copyright (C) 2009 Nokia Corporation
# Copyright (C) 2009-2010 Collabora Ltd.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of the GNU Lesser General Public
# License as published by the Free Software Foundation; either
# version 2.1 of the License, or (at your option) any later version.
#
# This library is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
# 02110-1301 USA

import dbus

from servicetest import (EventPattern, tp_name_prefix, tp_path_prefix,
        call_async, assertEquals)
from mctest import exec_test, SimulatedConnection, create_fakecm_account
import constants as cs

def test(q, bus, mc):
    # Create an account. We're setting register=True here to verify
    # that after one successful connection, it'll be removed (fd.o #28118).
    params = dbus.Dictionary({"account": "someguy@example.com",
        "password": "secrecy",
        "register": True}, signature='sv')
    (simulated_cm, account) = create_fakecm_account(q, bus, mc, params)

    call_async(q, account.Properties, 'Set', cs.ACCOUNT, 'Enabled', False)
    q.expect('dbus-return', method='Set')

    # Enable the account
    call_async(q, account.Properties, 'Set', cs.ACCOUNT, 'Enabled', True)

    # Set online presence
    presence = dbus.Struct((dbus.UInt32(cs.PRESENCE_BUSY), 'busy',
            'Fixing MC bugs'), signature='uss')
    call_async(q, account.Properties, 'Set', cs.ACCOUNT,
            'RequestedPresence', presence)

    e = q.expect('dbus-method-call', method='RequestConnection',
            args=['fakeprotocol', params],
            destination=tp_name_prefix + '.ConnectionManager.fakecm',
            path=tp_path_prefix + '/ConnectionManager/fakecm',
            interface=tp_name_prefix + '.ConnectionManager',
            handled=False)

    conn = SimulatedConnection(q, bus, 'fakecm', 'fakeprotocol', '_',
            'myself', has_presence=True)

    q.dbus_return(e.message, conn.bus_name, conn.object_path, signature='so')

    # MC prepares the connection, does any pre-Connect setup, then
    # calls Connect
    q.expect('dbus-method-call', method='Connect',
            path=conn.object_path, handled=True)

    # Connect succeeds
    conn.StatusChanged(cs.CONN_STATUS_CONNECTED, cs.CSR_NONE_SPECIFIED)

    q.expect('dbus-method-call',
             interface=cs.CONN_IFACE_PRESENCE,
             method='SetPresence',
             args=list(presence[1:]),
             handled=True)

    # Connection falls over for a miscellaneous reason
    conn.ConnectionError('com.example.My.Network.Is.Full.Of.Eels',
            {'eels': 23, 'capacity': 23, 'debug-message': 'Too many eels'})
    conn.StatusChanged(cs.CONN_STATUS_DISCONNECTED,
            cs.CSR_NETWORK_ERROR)

    # MC reconnects. This time, we expect it to have deleted the 'register'
    # parameter.
    del params['register']

    disconnected, connecting, e = q.expect_many(
            EventPattern('dbus-signal', signal='PropertiesChanged',
                predicate=(lambda e:
                    e.args[1].get('ConnectionStatus') ==
                        cs.CONN_STATUS_DISCONNECTED),
                ),
            EventPattern('dbus-signal', signal='PropertiesChanged',
                predicate=(lambda e:
                    e.args[1].get('ConnectionStatus') ==
                        cs.CONN_STATUS_CONNECTING),
                ),
            EventPattern('dbus-method-call', method='RequestConnection',
                args=['fakeprotocol', params],
                destination=tp_name_prefix + '.ConnectionManager.fakecm',
                path=tp_path_prefix + '/ConnectionManager/fakecm',
                interface=tp_name_prefix + '.ConnectionManager',
                handled=False),
            )

    assertEquals('/', disconnected.args[1].get('Connection'))
    assertEquals('com.example.My.Network.Is.Full.Of.Eels',
            disconnected.args[1].get('ConnectionError'))
    assertEquals(
            {'eels': 23, 'capacity': 23, 'debug-message': 'Too many eels'},
            disconnected.args[1].get('ConnectionErrorDetails'))
    assertEquals(cs.CONN_STATUS_DISCONNECTED,
        disconnected.args[1].get('ConnectionStatus'))
    assertEquals(cs.CSR_NETWORK_ERROR,
        disconnected.args[1].get('ConnectionStatusReason'))

    assertEquals('/', connecting.args[1].get('Connection'))
    assertEquals('com.example.My.Network.Is.Full.Of.Eels',
            connecting.args[1].get('ConnectionError'))
    assertEquals(
            {'eels': 23, 'capacity': 23, 'debug-message': 'Too many eels'},
            connecting.args[1].get('ConnectionErrorDetails'))
    assertEquals(cs.CONN_STATUS_CONNECTING,
        connecting.args[1].get('ConnectionStatus'))
    assertEquals(cs.CSR_REQUESTED,
        connecting.args[1].get('ConnectionStatusReason'))

    # The object path needs to be different from the first simulated
    # connection which we made above, because the object isn't removed
    # from this bus and it's actually hard to do so because it's not
    # really on a bus, it's on the queue. So let's just change the
    # object path and it's fine.
    conn = SimulatedConnection(q, bus, 'fakecm', 'fakeprotocol', 'second',
            'myself', has_presence=True)

    q.dbus_return(e.message, conn.bus_name, conn.object_path, signature='so')

    # MC prepares the connection, does any pre-Connect setup, then
    # calls Connect
    connecting, _ = q.expect_many(
            EventPattern('dbus-signal', signal='PropertiesChanged',
                predicate=(lambda e:
                    e.args[1].get('ConnectionStatus') ==
                        cs.CONN_STATUS_CONNECTING),
                ),
            EventPattern('dbus-method-call', method='Connect',
                path=conn.object_path, handled=True),
            )

    assertEquals(conn.object_path, connecting.args[1].get('Connection'))
    assertEquals('com.example.My.Network.Is.Full.Of.Eels',
            connecting.args[1].get('ConnectionError'))
    assertEquals(
            {'eels': 23, 'capacity': 23, 'debug-message': 'Too many eels'},
            connecting.args[1].get('ConnectionErrorDetails'))
    assertEquals(cs.CONN_STATUS_CONNECTING,
        connecting.args[1].get('ConnectionStatus'))
    assertEquals(cs.CSR_REQUESTED,
        connecting.args[1].get('ConnectionStatusReason'))

    assertEquals('com.example.My.Network.Is.Full.Of.Eels',
            account.Properties.Get(cs.ACCOUNT, 'ConnectionError'))
    assertEquals(
            {'eels': 23, 'capacity': 23, 'debug-message': 'Too many eels'},
            account.Properties.Get(cs.ACCOUNT, 'ConnectionErrorDetails'))

    # Connect succeeds
    conn.StatusChanged(cs.CONN_STATUS_CONNECTED, cs.CSR_NONE_SPECIFIED)

    connected, _ = q.expect_many(
            EventPattern('dbus-signal', signal='PropertiesChanged',
                predicate=(lambda e:
                    e.args[1].get('ConnectionStatus') ==
                        cs.CONN_STATUS_CONNECTED),
                ),
            EventPattern('dbus-method-call',
                interface=cs.CONN_IFACE_PRESENCE,
                method='SetPresence',
                args=list(presence[1:]),
                handled=True),
            )

    assertEquals(conn.object_path, connected.args[1].get('Connection'))
    assertEquals('', connected.args[1].get('ConnectionError'))
    assertEquals({}, connected.args[1].get('ConnectionErrorDetails'))
    assertEquals(cs.CONN_STATUS_CONNECTED,
        connected.args[1].get('ConnectionStatus'))
    assertEquals(cs.CSR_REQUESTED,
        connected.args[1].get('ConnectionStatusReason'))

    assertEquals('', account.Properties.Get(cs.ACCOUNT, 'ConnectionError'))
    assertEquals({}, account.Properties.Get(cs.ACCOUNT, 'ConnectionErrorDetails'))

if __name__ == '__main__':
    exec_test(test, {})
