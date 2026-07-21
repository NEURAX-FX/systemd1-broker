# SPDX-License-Identifier: LGPL-2.1-or-later

systemd1_broker_sources = files(
        'systemd1-broker-bus.c',
        'systemd1-broker-dbus.c',
        'systemd1-broker-metadata.c',
        'systemd1-broker.c',
)

systemd1_broker_backend_servicectl = shared_library(
        'systemd1-broker-backend-servicectl',
        files('systemd1-broker-backend-servicectl.c'),
        include_directories : includes + include_directories('.'),
        install : false,
        build_by_default : want_tests != 'false')

executables += [
        executable_template + {
                'name' : 'systemd1-broker',
                'sources' : systemd1_broker_sources + files('systemd1-broker-main.c'),
                'install' : false,
        },
]
