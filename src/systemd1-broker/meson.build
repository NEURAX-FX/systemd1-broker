# SPDX-License-Identifier: LGPL-2.1-or-later

systemd1_broker_sources = files(
        'systemd1-broker-bus.c',
        'systemd1-broker-dbus.c',
        'systemd1-broker.c',
)

executables += [
        executable_template + {
                'name' : 'systemd1-broker',
                'sources' : systemd1_broker_sources + files('systemd1-broker-main.c'),
                'install' : false,
        },
]
