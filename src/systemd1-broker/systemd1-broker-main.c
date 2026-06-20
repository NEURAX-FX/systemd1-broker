/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <getopt.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "alloc-util.h"
#include "build.h"
#include "errno-util.h"
#include "fd-util.h"
#include "log.h"
#include "main-func.h"
#include "mkdir.h"
#include "path-util.h"
#include "socket-util.h"
#include "systemd1-broker-dbus.h"
#include "systemd1-broker.h"

static const char *arg_socket = NULL;

static int help(void) {
        printf("%s [OPTIONS...]\n\n"
               "Serve a minimal org.freedesktop.systemd1 compatibility broker.\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n"
               "     --socket=PATH      Listen on this Unix stream socket\n",
               program_invocation_short_name);

        return 0;
}

static int parse_argv(int argc, char *argv[]) {
        enum {
                ARG_VERSION = 0x100,
                ARG_SOCKET,
        };

        static const struct option options[] = {
                { "help",    no_argument,       NULL, 'h'         },
                { "version", no_argument,       NULL, ARG_VERSION },
                { "socket",  required_argument, NULL, ARG_SOCKET  },
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

static int run(int argc, char *argv[]) {
        _cleanup_(systemd1_broker_manager_freep) Systemd1BrokerManager *manager = NULL;
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

        while (!quit) {
                connection_fd = RET_NERRNO(accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC));
                if (ERRNO_IS_NEG_ACCEPT_AGAIN(connection_fd))
                        continue;
                if (connection_fd < 0)
                        return log_error_errno(connection_fd, "Failed to accept connection on %s: %m", arg_socket);

                r = systemd1_broker_serve_bus_fd(TAKE_FD(connection_fd), manager, &quit);
                if (r < 0)
                        return log_error_errno(r, "Failed to serve D-Bus connection: %m");
        }

        return 0;
}

DEFINE_MAIN_FUNCTION(run);
