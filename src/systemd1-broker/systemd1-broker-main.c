/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "sd-bus.h"

#include "alloc-util.h"
#include "build.h"
#include "errno-util.h"
#include "fd-util.h"
#include "io-util.h"
#include "log.h"
#include "main-func.h"
#include "mkdir.h"
#include "path-util.h"
#include "socket-util.h"
#include "string-util.h"
#include "systemd1-broker-dbus.h"
#include "systemd1-broker.h"
#include "time-util.h"

static const char *arg_socket = NULL;
static const char *arg_bus_address = NULL;

static int help(void) {
        printf("%s [OPTIONS...]\n\n"
               "Serve a minimal org.freedesktop.systemd1 compatibility broker.\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n"
               "     --socket=PATH      Listen on this Unix stream socket\n"
               "     --bus-address=ADDR Connect to this D-Bus bus and own org.freedesktop.systemd1\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_SOCKET,
                ARG_BUS_ADDRESS,
        };

        static const struct option options[] = {
                { "help",        no_argument,       NULL, 'h'             },
                { "version",     no_argument,       NULL, ARG_VERSION     },
                { "socket",      required_argument, NULL, ARG_SOCKET      },
                { "bus-address", required_argument, NULL, ARG_BUS_ADDRESS },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "h", options, NULL)) >= 0)
                switch (c) {
                case 'h':
                        return help();

                case ARG_VERSION:
                        return version();

                case ARG_SOCKET:
                        arg_socket = optarg;
                        break;

                case ARG_BUS_ADDRESS:
                        arg_bus_address = optarg;
                        break;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached();
                }

        if (optind < argc)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "%s takes no positional arguments.", program_invocation_short_name);

        if (!arg_socket)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "--socket=PATH is required.");

        if (!path_is_absolute(arg_socket))
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "--socket=PATH must be absolute.");

        if (isempty(arg_bus_address))
                arg_bus_address = NULL;

        return 1;
}

static int make_listen_socket(const char *path) {
        _cleanup_close_ int fd = -EBADF;
        _cleanup_free_ char *directory = NULL;
        union sockaddr_union sa = {};
        int r, salen;

        assert(path);

        r = path_extract_directory(path, &directory);
        if (r < 0)
                return log_error_errno(r, "Failed to determine parent directory of %s: %m", path);

        r = mkdir_parents(directory, 0755);
        if (r < 0)
                return log_error_errno(r, "Failed to create parent directories for %s: %m", path);

        salen = sockaddr_un_set_path(&sa.un, path);
        if (salen < 0)
                return log_error_errno(salen, "Invalid socket path %s: %m", path);

        (void) sockaddr_un_unlink(&sa.un);

        fd = socket(AF_UNIX, SOCK_STREAM|SOCK_CLOEXEC, 0);
        if (fd < 0)
                return log_error_errno(errno, "Failed to allocate AF_UNIX socket: %m");

        if (bind(fd, &sa.sa, (socklen_t) salen) < 0)
                return log_error_errno(errno, "Failed to bind %s: %m", path);

        if (listen(fd, SOMAXCONN) < 0)
                return log_error_errno(errno, "Failed to listen on %s: %m", path);

        return TAKE_FD(fd);
}

static int connect_bus_address(const char *address, Systemd1BrokerManager *manager, sd_bus **ret) {
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *bus = NULL;
        int r;

        assert(address);
        assert(manager);
        assert(ret);

        r = sd_bus_new(&bus);
        if (r < 0)
                return r;

        r = sd_bus_set_address(bus, address);
        if (r < 0)
                return r;

        r = sd_bus_set_bus_client(bus, true);
        if (r < 0)
                return r;

        r = systemd1_broker_dbus_add_manager(bus, manager);
        if (r < 0)
                return r;

        r = sd_bus_start(bus);
        if (r < 0)
                return r;

        r = sd_bus_request_name(bus, "org.freedesktop.systemd1", SD_BUS_NAME_REPLACE_EXISTING|SD_BUS_NAME_ALLOW_REPLACEMENT);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(bus);
        return 0;
}

static int process_bus(sd_bus *bus) {
        int r;

        if (!bus)
                return 0;

        do {
                r = sd_bus_process(bus, NULL);
                if (ERRNO_IS_NEG_DISCONNECT(r))
                        return 0;
                if (r < 0)
                        return r;
        } while (r > 0);

        return 0;
}

static int bus_poll_timeout(sd_bus *bus, usec_t *ret) {
        uint64_t timeout;
        int r;

        assert(ret);

        if (!bus) {
                *ret = UINT64_MAX;
                return 0;
        }

        r = sd_bus_get_timeout(bus, &timeout);
        if (r < 0)
                return r;

        *ret = usec_sub_unsigned(timeout, now(CLOCK_MONOTONIC));
        return 0;
}

static int run(int argc, char *argv[]) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
        _cleanup_(sd_bus_flush_close_unrefp) sd_bus *system_bus = NULL;
        _cleanup_close_ int listen_fd = -EBADF, connection_fd = -EBADF;
        bool quit = false;
        int r;

        log_setup();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r;

        r = systemd1_broker_manager_new(&manager);
        if (r < 0)
                return log_error_errno(r, "Failed to allocate broker manager: %m");

        r = systemd1_broker_manager_add_unit(manager, "alpha.service", "Alpha", NULL);
        if (r < 0)
                return log_error_errno(r, "Failed to add static alpha.service: %m");

        listen_fd = make_listen_socket(arg_socket);
        if (listen_fd < 0)
                return listen_fd;

        if (arg_bus_address) {
                r = connect_bus_address(arg_bus_address, manager, &system_bus);
                if (r < 0)
                        return log_error_errno(r, "Failed to connect to D-Bus bus at %s: %m", arg_bus_address);
        }

        while (!quit) {
                usec_t timeout;

                r = process_bus(system_bus);
                if (r < 0)
                        return log_error_errno(r, "Failed to process D-Bus bus connection: %m");

                r = bus_poll_timeout(system_bus, &timeout);
                if (r < 0)
                        return log_error_errno(r, "Failed to query D-Bus bus timeout: %m");

                struct pollfd pollfd[] = {
                        { .fd = listen_fd, .events = POLLIN },
                        { .fd = system_bus ? sd_bus_get_fd(system_bus) : -EBADF, .events = system_bus ? sd_bus_get_events(system_bus) : 0 },
                };

                r = ppoll_usec(pollfd, ELEMENTSOF(pollfd), timeout);
                if (r < 0) {
                        if (ERRNO_IS_TRANSIENT(r))
                                continue;

                        return log_error_errno(r, "Failed to poll broker sockets: %m");
                }

                if (pollfd[0].revents != 0) {
                        connection_fd = RET_NERRNO(accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC));
                        if (ERRNO_IS_NEG_ACCEPT_AGAIN(connection_fd))
                                continue;
                        if (connection_fd < 0)
                                return log_error_errno(connection_fd, "Failed to accept connection on %s: %m", arg_socket);

                        r = systemd1_broker_serve_bus_fd(TAKE_FD(connection_fd), manager, &quit);
                        if (r < 0)
                                return log_error_errno(r, "Failed to serve D-Bus connection: %m");
                }
        }

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
