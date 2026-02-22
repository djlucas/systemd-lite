/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef LINUX_REBOOT_CMD_POWER_OFF
#include <linux/reboot.h>
#endif

#define DEFAULT_MANAGER_PATH "/usr/lib/systemd/systemd-smd"
#define DEFAULT_SHUTDOWN_TIMEOUT_SEC 90
#define DEFAULT_CONFIG_PATH "/etc/systemd/init.conf"

static int log_fd = -1;
static int shutdown_timeout_sec = DEFAULT_SHUTDOWN_TIMEOUT_SEC;
static const char *config_path = DEFAULT_CONFIG_PATH;

typedef enum ManagerExitPolicy {
        MANAGER_EXIT_REBOOT,
        MANAGER_EXIT_POWEROFF,
        MANAGER_EXIT_HALT,
        MANAGER_EXIT_RESPAWN,
        MANAGER_EXIT_FREEZE,
} ManagerExitPolicy;

static ManagerExitPolicy manager_exit_policy = MANAGER_EXIT_REBOOT;

static void log_open(void) {
        if (log_fd >= 0)
                return;

        log_fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC | O_NOCTTY);
        if (log_fd < 0)
                log_fd = STDERR_FILENO;
}

static void log_msg(const char *level, const char *fmt, ...) {
        char buf[1024];
        va_list ap;
        int n;

        log_open();

        va_start(ap, fmt);
        n = vsnprintf(buf, sizeof(buf), fmt, ap);
        va_end(ap);

        if (n < 0)
                return;

        if (log_fd == STDERR_FILENO) {
                dprintf(log_fd, "systemd-init: %s\n", buf);
                return;
        }

        dprintf(log_fd, "%s%s%s\n", level, "systemd-init: ", buf);
}

static void log_errno(const char *msg) {
        log_msg("<3>", "%s: %s", msg, strerror(errno));
}

static void reap_children(pid_t manager_pid, bool *manager_exited, int *manager_status) {
        for (;;) {
                int status;
                pid_t pid = waitpid(-1, &status, WNOHANG);

                if (pid == 0)
                        return;
                if (pid < 0) {
                        if (errno == ECHILD)
                                return;
                        log_errno("waitpid failed");
                        return;
                }

                if (pid == manager_pid) {
                        *manager_exited = true;
                        *manager_status = status;
                }
        }
}

static int reboot_command_for_signal(int sig) {
        switch (sig) {
        case SIGTERM:
        case SIGPWR:
                return LINUX_REBOOT_CMD_POWER_OFF;
        case SIGINT:
        case SIGUSR1:
                return LINUX_REBOOT_CMD_RESTART;
        case SIGUSR2:
                return LINUX_REBOOT_CMD_HALT;
        default:
                return LINUX_REBOOT_CMD_RESTART;
        }
}

static bool is_shutdown_signal(int sig) {
        return sig == SIGTERM || sig == SIGINT || sig == SIGPWR;
}

static bool invoked_as(const char *argv0, const char *name) {
        const char *p;

        if (!argv0 || !name)
                return false;

        p = strrchr(argv0, '/');
        p = p ? p + 1 : argv0;

        return strcmp(p, name) == 0;
}

static void redirect_telinit(int argc, char *argv[]) {
#if HAVE_SYSV_COMPAT
        if (getpid() == 1)
                return;

        if (!invoked_as(argv[0], "init") && !invoked_as(argv[0], "telinit"))
                return;

        execv(SYSTEMCTL_BINARY_PATH, argv);
        log_errno("Failed to exec " SYSTEMCTL_BINARY_PATH);
        exit(EXIT_FAILURE);
#else
        (void) argc;
        (void) argv;
#endif
}

static int64_t now_monotonic_usec(void) {
        struct timespec ts;
        if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
                return -1;
        return (int64_t) ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int build_manager_argv(const char *manager, int argc, char *argv[], int start_index, char ***ret_argv) {
        int count = argc - start_index + 1; /* +1 for manager */
        char **child_argv = calloc((size_t) count + 1, sizeof(char *));
        if (!child_argv)
                return -1;

        child_argv[0] = (char *) manager;
        for (int i = start_index, j = 1; i < argc; i++, j++)
                child_argv[j] = argv[i];
        child_argv[count] = NULL;

        *ret_argv = child_argv;
        return 0;
}

static pid_t spawn_manager(const char *manager, char **child_argv) {
        pid_t pid = fork();
        if (pid < 0) {
                log_errno("fork failed");
                return -1;
        }
        if (pid == 0) {
                sigset_t empty;
                sigemptyset(&empty);
                sigprocmask(SIG_SETMASK, &empty, NULL);

                execv(manager, child_argv ? child_argv : (char * const[]) { (char *) manager, NULL });
                dprintf(STDERR_FILENO, "systemd-init: Failed to exec %s: %s\n", manager, strerror(errno));
                _exit(127);
        }

        return pid;
}

static ManagerExitPolicy manager_exit_policy_from_string(const char *s) {
        if (!s)
                return MANAGER_EXIT_REBOOT;
        if (strcmp(s, "reboot") == 0)
                return MANAGER_EXIT_REBOOT;
        if (strcmp(s, "poweroff") == 0)
                return MANAGER_EXIT_POWEROFF;
        if (strcmp(s, "halt") == 0)
                return MANAGER_EXIT_HALT;
        if (strcmp(s, "respawn") == 0)
                return MANAGER_EXIT_RESPAWN;
        if (strcmp(s, "freeze") == 0)
                return MANAGER_EXIT_FREEZE;
        return MANAGER_EXIT_REBOOT;
}

static int reboot_command_for_policy(ManagerExitPolicy policy) {
        switch (policy) {
        case MANAGER_EXIT_POWEROFF:
                return LINUX_REBOOT_CMD_POWER_OFF;
        case MANAGER_EXIT_HALT:
                return LINUX_REBOOT_CMD_HALT;
        case MANAGER_EXIT_REBOOT:
        default:
                return LINUX_REBOOT_CMD_RESTART;
        }
}

static char *trim(char *s) {
        char *e;

        if (!s)
                return s;

        while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
                s++;

        if (*s == '\0')
                return s;

        e = s + strlen(s);
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r'))
                e--;
        *e = '\0';
        return s;
}

static void load_config_file(const char *path) {
        FILE *f;
        char line[512];

        f = fopen(path, "re");
        if (!f)
                return;

        while (fgets(line, sizeof(line), f)) {
                char *eq, *key, *val;

                if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
                        continue;

                eq = strchr(line, '=');
                if (!eq)
                        continue;
                *eq = '\0';
                key = trim(line);
                val = trim(eq + 1);

                if (key[0] == '\0')
                        continue;

                if (strcmp(key, "ShutdownTimeoutSec") == 0 && val[0]) {
                        char *end = NULL;
                        long t = strtol(val, &end, 10);
                        if (end && *end == '\0' && t >= 0)
                                shutdown_timeout_sec = (int) t;
                } else if (strcmp(key, "OnManagerExit") == 0 && val[0]) {
                        manager_exit_policy = manager_exit_policy_from_string(val);
                }
        }

        fclose(f);
}

int main(int argc, char *argv[]) {
        const char *manager = DEFAULT_MANAGER_PATH;
        char **child_argv = NULL;
        int manager_status = 0;
        pid_t manager_pid;
        int shutdown_cmd = -1;
        int64_t shutdown_deadline = -1;
        bool manager_exited = false;
        bool shutdown_requested = false;
        int sfd;
        sigset_t mask;
        int argi = 1;

        redirect_telinit(argc, argv);

        if (getpid() != 1) {
                log_msg("<3>", "Refusing to run as pid %d (not PID 1)", getpid());
                return 1;
        }

        /* First pass: find optional --config= */
        for (int i = 1; i < argc; i++) {
                if (strncmp(argv[i], "--config=", 9) == 0) {
                        config_path = argv[i] + 9;
                        break;
                }
                if (strcmp(argv[i], "--") == 0)
                        break;
        }

        load_config_file(config_path);

        /* Second pass: apply overrides and find manager argv start */
        for (; argi < argc; argi++) {
                if (strncmp(argv[argi], "--shutdown-timeout=", 19) == 0) {
                        const char *v = argv[argi] + 19;
                        char *end = NULL;
                        long t = strtol(v, &end, 10);
                        if (!v[0] || (end && *end != '\0') || t < 0)
                                log_msg("<3>", "Invalid --shutdown-timeout=%s, using default", v);
                        else
                                shutdown_timeout_sec = (int) t;
                        continue;
                }
                if (strncmp(argv[argi], "--on-manager-exit=", 19) == 0) {
                        const char *v = argv[argi] + 19;
                        ManagerExitPolicy p = manager_exit_policy_from_string(v);
                        if (!v[0])
                                log_msg("<3>", "Invalid --on-manager-exit=, using default");
                        else
                                manager_exit_policy = p;
                        continue;
                }
                if (strcmp(argv[argi], "--") == 0) {
                        argi++;
                        break;
                }
                break;
        }

        if (argi < argc) {
            if (build_manager_argv(manager, argc, argv, argi, &child_argv) < 0) {
                log_errno("Failed to allocate manager argv");
                return 1;
            }
        }

        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGTERM);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGPWR);
        sigaddset(&mask, SIGHUP);
        sigaddset(&mask, SIGUSR1);
        sigaddset(&mask, SIGUSR2);

        if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0) {
                log_errno("sigprocmask failed");
                return 1;
        }

        sfd = signalfd(-1, &mask, SFD_CLOEXEC);
        if (sfd < 0) {
                log_errno("signalfd failed");
                return 1;
        }

        log_msg("<6>", "Starting manager %s", manager);
        manager_pid = spawn_manager(manager, child_argv);
        if (manager_pid < 0)
                return 1;

        for (;;) {
                struct signalfd_siginfo si;
                ssize_t n;
                int timeout_ms = -1;

                if (shutdown_requested && !manager_exited && shutdown_deadline > 0) {
                        int64_t now = now_monotonic_usec();
                        if (now > 0) {
                                int64_t remaining = shutdown_deadline - now;
                                if (remaining <= 0)
                                        break;
                                timeout_ms = (int) ((remaining + 999) / 1000);
                        }
                }

                if (timeout_ms >= 0) {
                        struct pollfd pfd = { .fd = sfd, .events = POLLIN };
                        int pr = poll(&pfd, 1, timeout_ms);
                        if (pr == 0)
                                break;
                        if (pr < 0) {
                                if (errno == EINTR)
                                        continue;
                                log_errno("poll failed");
                                continue;
                        }
                }

                n = read(sfd, &si, sizeof(si));
                if (n < 0) {
                        if (errno == EINTR)
                                continue;
                        log_errno("signalfd read failed");
                        continue;
                }
                if ((size_t) n < sizeof(si))
                        continue;

                switch (si.ssi_signo) {
                case SIGCHLD:
                        reap_children(manager_pid, &manager_exited, &manager_status);
                        break;
                case SIGHUP:
                case SIGTERM:
                case SIGINT:
                case SIGPWR:
                case SIGUSR1:
                case SIGUSR2:
                        if (is_shutdown_signal((int) si.ssi_signo)) {
                                shutdown_cmd = reboot_command_for_signal((int) si.ssi_signo);
                                shutdown_requested = true;
                                if (shutdown_deadline < 0) {
                                        int64_t now = now_monotonic_usec();
                                        if (now > 0)
                                                shutdown_deadline = now + (int64_t) shutdown_timeout_sec * 1000000;
                                }
                                if (!manager_exited)
                                        kill(manager_pid, (int) si.ssi_signo);
                        } else if (!manager_exited) {
                                kill(manager_pid, (int) si.ssi_signo);
                        }
                        break;
                default:
                        break;
                }

                if (manager_exited) {
                        if (!shutdown_requested && manager_exit_policy == MANAGER_EXIT_RESPAWN) {
                                log_msg("<3>", "Manager exited; respawning");
                                sleep(1);
                                manager_pid = spawn_manager(manager, child_argv);
                                if (manager_pid < 0)
                                        return 1;
                                manager_exited = false;
                                manager_status = 0;
                                continue;
                        }
                        break;
                }
        }

        if (shutdown_requested && !manager_exited)
                log_msg("<3>", "Shutdown timed out, forcing reboot");

        if (!shutdown_requested) {
                switch (manager_exit_policy) {
                case MANAGER_EXIT_FREEZE:
                        log_msg("<3>", "Manager exited; freezing");
                        for (;;)
                                pause();
                case MANAGER_EXIT_RESPAWN:
                        log_msg("<3>", "Manager exited; forcing reboot");
                        shutdown_cmd = LINUX_REBOOT_CMD_RESTART;
                        break;
                case MANAGER_EXIT_POWEROFF:
                case MANAGER_EXIT_HALT:
                case MANAGER_EXIT_REBOOT:
                default:
                        log_msg("<3>", "Manager exited; forcing reboot");
                        shutdown_cmd = reboot_command_for_policy(manager_exit_policy);
                        break;
                }
        }

        if (manager_status != 0)
                log_msg("<3>", "Manager status: 0x%x", manager_status);

        sync();
        reboot(shutdown_cmd);

        log_errno("reboot failed");
        for (;;)
                pause();

        return 1;
}
