#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <limits.h>
#include <netdb.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef VERSION
#define VERSION "dev"
#endif

typedef enum {
    PATH_AUTO,
    PATH_RELATIVE,
    PATH_ABSOLUTE
} path_mode_t;

static int sleep_sec = 0;
static int timeout_sec = 0;
static int retries = 0;
static int interval_sec = 1;
static int count_sec = 0;

static bool quiet = false;
static bool verbose = false;

static char *ready_file = NULL;
static char *wait_port = NULL;

static path_mode_t path_mode = PATH_AUTO;

static char *success_match = NULL;
static char *retry_match = NULL;
static char *success_string = NULL;
static char *retry_string = NULL;

#define MAX_EXIT_CODES 32
static int success_exit_codes[MAX_EXIT_CODES];
static int retry_exit_codes[MAX_EXIT_CODES];
static int success_exit_count = 0;
static int retry_exit_count = 0;

static bool has_success_exit = false;
static bool has_retry_exit = false;

static void logmsg(const char *fmt, ...) {
    if (quiet) return;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y/%m/%d %I:%M:%S%p", &tm_now);
    fprintf(stderr, "%s INF ", ts);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void countmsg(const char *fmt, ...) {
    if (quiet || count_sec <= 0) return;
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    char ts[64];
    strftime(ts, sizeof(ts), "%Y/%m/%d %I:%M:%S%p", &tm_now);
    fprintf(stderr, "%s INF ", ts);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static void usage(void) {
    printf(
"Usage: delayme [options] <program> [args...]\n\n"
"\n"
"Options:\n"
"\n"
"  -s, --sleep SEC          Initial delay before first run\n"
"  -t, --timeout SEC        Kill child if it runs longer than this\n"
"  -r, --retries N          Retry up to N times on failure\n"
"  -i, --interval SEC       Seconds between retries\n"
"  -c, --count SEC          Show countdown every N seconds\n"
"  --ready-file PATH        Wait until file exists\n"
"  --wait-port HOST:PORT    Wait until TCP port is open\n"
"  --relative               Resolve executable relative to delayme binary\n"
"  --absolute               Require absolute path to executable\n"
"  --success-match REGEX    Success if output matches regex\n"
"  --retry-match REGEX      Retry if output matches regex\n"
"  --success-string TEXT    Success if output contains literal string\n"
"  --retry-string TEXT      Retry if output contains literal string\n"
"  --success-exit CODES     Comma-separated success exit codes\n"
"  --retry-exit CODES       Comma-separated retry-only exit codes\n"
"  -q, --quiet              No output\n"
"  -v, --verbose            More verbose logging\n"
"  -h, --help\n"
"  --version\n"
    );
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool port_open(const char *hostport) {
    char host[256];
    char portstr[32];

    if (sscanf(hostport, "%255[^:]:%31s", host, portstr) != 2)
        return false;

    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    struct addrinfo *rp;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    int ret = getaddrinfo(host, portstr, &hints, &result);
    if (ret != 0) return false;

    bool success = false;
    for (rp = result; rp != NULL; rp = rp->ai_next) {
        int sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) continue;
        if (connect(sock, rp->ai_addr, rp->ai_addrlen) == 0) {
            success = true;
            close(sock);
            break;
        }
        close(sock);
    }
    freeaddrinfo(result);
    return success;
}

static bool regex_match(const char *pattern, const char *text) {
    regex_t regex;
    if (regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) != 0)
        return false;
    bool matched = regexec(&regex, text, 0, NULL, 0) == 0;
    regfree(&regex);
    return matched;
}

static void parse_exit_codes(const char *str, int *codes, int *count) {
    char *copy = strdup(str);
    if (!copy) return;
    char *token = strtok(copy, ",");
    while (token && *count < MAX_EXIT_CODES) {
        codes[*count] = atoi(token);
        (*count)++;
        token = strtok(NULL, ",");
    }
    free(copy);
}

static bool exit_code_matches(int code, int *codes, int count) {
    for (int i = 0; i < count; i++) {
        if (codes[i] == code) return true;
    }
    return false;
}

static char *resolve_command(char *cmd) {
    static char resolved[PATH_MAX];

    if (path_mode == PATH_ABSOLUTE) {
        if (cmd[0] != '/') {
            fprintf(stderr, "absolute path required: %s\n", cmd);
            exit(125);
        }
        return cmd;
    }

    if (path_mode == PATH_RELATIVE) {
        char exe[PATH_MAX];
        ssize_t len = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (len < 0) {
            perror("readlink");
            exit(125);
        }
        exe[len] = '\0';
        char *dir = dirname(exe);
        snprintf(resolved, sizeof(resolved), "%s/%s", dir, cmd[0] == '/' ? cmd + 1 : cmd);
        return resolved;
    }

    return cmd;  /* PATH_AUTO - normal execvp behavior */
}

static void format_command(char *buf, size_t size, char **argv, int start) {
    buf[0] = '\0';
    for (int i = start; argv[i]; i++) {
        if (i > start) strncat(buf, " ", size - strlen(buf) - 1);
        strncat(buf, argv[i], size - strlen(buf) - 1);
    }
}

/* ====================== CHILD EXECUTION ====================== */

typedef struct {
    int exit_code;
    char output[8192];
} run_result_t;

static run_result_t run_child(char **argv, int optind, bool capture) {
    run_result_t res = { .exit_code = 125, .output = {0} };
    int stdout_pipe[2] = {-1,-1}, stderr_pipe[2] = {-1,-1};
    size_t out_len = 0;

    if (capture) {
        if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) {
            perror("pipe");
            return res;
        }
    }

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return res;
    }

    if (pid == 0) {
        if (capture) {
            close(stdout_pipe[0]);
            close(stderr_pipe[0]);
            dup2(stdout_pipe[1], STDOUT_FILENO);
            dup2(stderr_pipe[1], STDERR_FILENO);
            close(stdout_pipe[1]);
            close(stderr_pipe[1]);
        }
        execvp(argv[optind], &argv[optind]);
        perror("execvp");
        _exit(125);
    }

    if (capture) {
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);
    }

    bool child_exited = false;
    time_t start_time = time(NULL);

    while (!child_exited || (capture && (stdout_pipe[0] >= 0 || stderr_pipe[0] >= 0))) {
        if (capture) {
            fd_set readfds;
            FD_ZERO(&readfds);
            int maxfd = -1;

            if (stdout_pipe[0] >= 0) {
                FD_SET(stdout_pipe[0], &readfds);
                if (stdout_pipe[0] > maxfd) maxfd = stdout_pipe[0];
            }
            if (stderr_pipe[0] >= 0) {
                FD_SET(stderr_pipe[0], &readfds);
                if (stderr_pipe[0] > maxfd) maxfd = stderr_pipe[0];
            }

            struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
            if (maxfd >= 0 && select(maxfd + 1, &readfds, NULL, NULL, &tv) > 0) {
                char buf[512];
                int fds[2] = {stdout_pipe[0], stderr_pipe[0]};
                for (int i = 0; i < 2; i++) {
                    int fd = fds[i];
                    if (fd >= 0 && FD_ISSET(fd, &readfds)) {
                        ssize_t n = read(fd, buf, sizeof(buf));
                        if (n > 0) {
                            // NO fwrite when capturing (per your request)
                            if (out_len + n < sizeof(res.output) - 1) {
                                memcpy(res.output + out_len, buf, n);
                                out_len += n;
                                res.output[out_len] = '\0';
                            }
                        } else if (n == 0) {
                            close(fd);
                            if (i == 0) stdout_pipe[0] = -1;
                            else stderr_pipe[0] = -1;
                        }
                    }
                }
            }
        }

        pid_t result = waitpid(pid, &res.exit_code, WNOHANG);
        if (result == pid) {
            child_exited = true;
            if (WIFEXITED(res.exit_code))
                res.exit_code = WEXITSTATUS(res.exit_code);
            else
                res.exit_code = 125;
        }

        if (timeout_sec > 0 && (time(NULL) - start_time) >= timeout_sec) {
            kill(pid, SIGKILL);
            waitpid(pid, NULL, 0);
            logmsg("timeout exceeded");
            res.exit_code = 124;
            return res;
        }
    }

    if (capture) {
        if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
        if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    }

    return res;
}

int main(int argc, char **argv) {
    int opt;
    static struct option long_opts[] = {
        {"sleep", required_argument, 0, 's'},
        {"timeout", required_argument, 0, 't'},
        {"retries", required_argument, 0, 'r'},
        {"interval", required_argument, 0, 'i'},
        {"count", required_argument, 0, 'c'},
        {"quiet", no_argument, 0, 'q'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 1},
        {"ready-file", required_argument, 0, 2},
        {"wait-port", required_argument, 0, 3},
        {"relative", no_argument, 0, 4},
        {"absolute", no_argument, 0, 5},
        {"success-match", required_argument, 0, 6},
        {"retry-match", required_argument, 0, 7},
        {"success-exit", required_argument, 0, 8},
        {"retry-exit", required_argument, 0, 9},
        {"success-string", required_argument, 0, 10},
        {"retry-string", required_argument, 0, 11},
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc, argv, "s:t:r:i:c:qvh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 's': sleep_sec = atoi(optarg); break;
            case 't': timeout_sec = atoi(optarg); break;
            case 'r': retries = atoi(optarg); break;
            case 'i': interval_sec = atoi(optarg); break;
            case 'c': count_sec = atoi(optarg); break;
            case 'q': quiet = true; break;
            case 'v': verbose = true; break;
            case 'h': usage(); return 0;
            case 1: printf("%s\n", VERSION); return 0;
            case 2: ready_file = optarg; break;
            case 3: wait_port = optarg; break;
            case 4: path_mode = PATH_RELATIVE; break;
            case 5: path_mode = PATH_ABSOLUTE; break;
            case 6: success_match = optarg; break;
            case 7: retry_match = optarg; break;
            case 8: has_success_exit = true; parse_exit_codes(optarg, success_exit_codes, &success_exit_count); break;
            case 9: has_retry_exit = true; parse_exit_codes(optarg, retry_exit_codes, &retry_exit_count); break;
            case 10: success_string = optarg; break;
            case 11: retry_string = optarg; break;
        }
    }

    if (optind >= argc) {
        usage();
        return 125;
    }

    char command[1024];
    format_command(command, sizeof(command), argv, optind);

    if (sleep_sec > 0) {
        int elapsed = 0;
        while (elapsed < sleep_sec) {
            int remain = sleep_sec - elapsed;
            int step = count_sec > 0 ? count_sec : remain;
            if (step > remain) step = remain;

            if (count_sec > 0) {
                if (verbose)
                    countmsg("DelayMe Wait %d: %s", remain, command);
                else
                    countmsg("%d", elapsed + step);
            }
            sleep(step);
            elapsed += step;
        }
    }

    if (ready_file) {
        int elapsed = 0;
        while (!file_exists(ready_file)) {
            int step = count_sec > 0 ? count_sec : interval_sec;
            elapsed += step;
            if (count_sec > 0) {
                if (verbose)
                    countmsg("DelayMe File Wait %d: %s", elapsed, command);
                else
                    countmsg("%d", elapsed);
            }
            sleep(step);
        }
        if (verbose) logmsg("DelayMe File Available Run %s", command);
    }

    if (wait_port) {
        int elapsed = 0;
        while (!port_open(wait_port)) {
            int step = count_sec > 0 ? count_sec : interval_sec;
            elapsed += step;
            if (count_sec > 0) {
                if (verbose)
                    countmsg("DelayMe Port Wait %d: %s", elapsed, command);
                else
                    countmsg("%d", elapsed);
            }
            sleep(step);
        }
        if (verbose) logmsg("DelayMe Port Available Run %s", command);
    }

    /* Only resolve when relative or absolute mode is explicitly requested */
    if (path_mode != PATH_AUTO) {
        argv[optind] = resolve_command(argv[optind]);
    }

    bool need_capture = success_match || retry_match || success_string || retry_string;
    int attempt = 0;

    while (1) {
        if (verbose) logmsg("DelayMe Run %s", command);

        run_result_t res = run_child(argv, optind, need_capture);

        bool success = false;
        if (has_success_exit && exit_code_matches(res.exit_code, success_exit_codes, success_exit_count))
            success = true;
        if (need_capture) {
            if (success_match && regex_match(success_match, res.output)) success = true;
            if (success_string && strstr(res.output, success_string)) success = true;
        }

        if (success) return 0;

        bool should_retry = false;
        if (has_retry_exit && exit_code_matches(res.exit_code, retry_exit_codes, retry_exit_count))
            should_retry = true;
        if (need_capture) {
            if (retry_match && regex_match(retry_match, res.output)) should_retry = true;
            if (retry_string && strstr(res.output, retry_string)) should_retry = true;
        }

        if (should_retry && attempt++ < retries) {
            sleep(interval_sec);
            continue;
        }

        return res.exit_code;
    }
}