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

static bool quiet = false;
static bool verbose = false;

static char *ready_file = NULL;
static char *wait_port = NULL;

static path_mode_t path_mode = PATH_AUTO;

static char *success_match = NULL;
static char *retry_match = NULL;

#define MAX_EXIT_CODES 32

static int success_exit_codes[MAX_EXIT_CODES];
static int retry_exit_codes[MAX_EXIT_CODES];

static int success_exit_count = 0;
static int retry_exit_count = 0;

static void logmsg(const char *fmt, ...) {
    if (quiet)
        return;

    va_list args;

    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fprintf(stderr, "\n");
    va_end(args);
}

static void usage(void) {
    printf(
"Usage:\n"
"  delayme [options] <program> [args...]\n"
"\n"
"Options:\n"
"\n"
"-s, --sleep SEC\n"
"        Initial delay before execution\n"
"\n"
"-t, --timeout SEC\n"
"        Kill child if it runs too long\n"
"\n"
"-r, --retries N\n"
"        Retry failures\n"
"\n"
"-i, --interval SEC\n"
"        Wait between retries\n"
"\n"
"--ready-file PATH\n"
"        Wait until file exists\n"
"\n"
"--wait-port HOST:PORT\n"
"        Wait until TCP port opens\n"
"\n"
"--relative\n"
"        Resolve executable relative to delayme binary\n"
"\n"
"--absolute\n"
"        Require absolute executable path\n"
"\n"
"--success-match STR\n"
"--retry-match STR\n"
"\n"
"--success-exit CODES\n"
"--retry-exit CODES\n"
"        Comma-separated exit codes\n"
"\n"
"-q, --quiet\n"
"-v, --verbose\n"
"\n"
"-h, --help\n"
"--version\n"
    );
}

static bool file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static bool port_open(const char *hostport) {
    char host[256];
    int port = 0;

    sscanf(hostport, "%255[^:]:%d", host, &port);

    int sock = socket(AF_INET, SOCK_STREAM, 0);

    if (sock < 0)
        return false;

    struct hostent *he = gethostbyname(host);

    if (!he) {
        close(sock);
        return false;
    }

    struct sockaddr_in addr = {0};

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int result =
        connect(sock,
                (struct sockaddr *)&addr,
                sizeof(addr));

    close(sock);

    return result == 0;
}

static bool regex_match(const char *pattern,
                        const char *text) {
    regex_t regex;

    if (regcomp(&regex,
                pattern,
                REG_EXTENDED | REG_NOSUB) != 0)
        return false;

    bool matched =
        regexec(&regex, text, 0, NULL, 0) == 0;

    regfree(&regex);

    return matched;
}

static void parse_exit_codes(const char *str,
                             int *codes,
                             int *count) {
    char *copy = strdup(str);

    if (!copy)
        return;

    char *token = strtok(copy, ",");

    while (token && *count < MAX_EXIT_CODES) {
        codes[*count] = atoi(token);
        (*count)++;
        token = strtok(NULL, ",");
    }

    free(copy);
}

static bool exit_code_matches(int code,
                              int *codes,
                              int count) {
    for (int i = 0; i < count; i++) {
        if (codes[i] == code)
            return true;
    }

    return false;
}

static char *resolve_command(char *argv0,
                             char *cmd) {
    static char resolved[PATH_MAX];

    if (path_mode == PATH_AUTO)
        return cmd;

    if (path_mode == PATH_ABSOLUTE) {
        if (cmd[0] != '/') {
            fprintf(stderr,
                    "absolute path required\n");
            exit(125);
        }

        return cmd;
    }

    char exe_path[PATH_MAX];

    ssize_t len =
        readlink("/proc/self/exe",
                 exe_path,
                 sizeof(exe_path) - 1);

    if (len < 0) {
        perror("readlink");
        exit(125);
    }

    exe_path[len] = '\0';

    char *dir = dirname(exe_path);

    snprintf(resolved,
             sizeof(resolved),
             "%s/%s",
             dir,
             cmd);

    return resolved;
}

int main(int argc, char **argv) {
    int opt;

    static struct option long_opts[] = {
        {"sleep", required_argument, 0, 's'},
        {"timeout", required_argument, 0, 't'},
        {"retries", required_argument, 0, 'r'},
        {"interval", required_argument, 0, 'i'},
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
        {0, 0, 0, 0}
    };

    while ((opt = getopt_long(argc,
                              argv,
                              "s:t:r:i:qvh",
                              long_opts,
                              NULL)) != -1) {
        switch (opt) {
            case 's':
                sleep_sec = atoi(optarg);
                break;

            case 't':
                timeout_sec = atoi(optarg);
                break;

            case 'r':
                retries = atoi(optarg);
                break;

            case 'i':
                interval_sec = atoi(optarg);
                break;

            case 'q':
                quiet = true;
                break;

            case 'v':
                verbose = true;
                break;

            case 'h':
                usage();
                return 0;

            case 1:
                printf("%s\n", VERSION);
                return 0;

            case 2:
                ready_file = optarg;
                break;

            case 3:
                wait_port = optarg;
                break;

            case 4:
                path_mode = PATH_RELATIVE;
                break;

            case 5:
                path_mode = PATH_ABSOLUTE;
                break;

            case 6:
                success_match = optarg;
                break;

            case 7:
                retry_match = optarg;
                break;

            case 8:
                parse_exit_codes(optarg,
                                 success_exit_codes,
                                 &success_exit_count);
                break;

            case 9:
                parse_exit_codes(optarg,
                                 retry_exit_codes,
                                 &retry_exit_count);
                break;
        }
    }

    if (optind >= argc) {
        usage();
        return 125;
    }

    if (sleep_sec > 0)
        sleep(sleep_sec);

    while (ready_file &&
           !file_exists(ready_file))
        sleep(interval_sec);

    while (wait_port &&
           !port_open(wait_port))
        sleep(interval_sec);

    char *cmd =
        resolve_command(argv[0],
                        argv[optind]);

    int attempt = 0;

    while (1) {
        int pipefd[2];

        if (pipe(pipefd) < 0) {
            perror("pipe");
            return 125;
        }

        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            return 125;
        }

        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);

            close(pipefd[0]);
            close(pipefd[1]);

            execvp(cmd, &argv[optind]);

            perror("execvp");
            exit(125);
        }

        close(pipefd[1]);

        char output[8192] = {0};

        ssize_t total = 0;

        fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

        int status;
        time_t start = time(NULL);

        while (1) {
            char buf[512];

            ssize_t n =
                read(pipefd[0],
                     buf,
                     sizeof(buf));

            if (n > 0) {
                if (total + n <
                    (ssize_t)sizeof(output) - 1) {
                    memcpy(output + total,
                           buf,
                           n);

                    total += n;
                    output[total] = '\0';
                }
            }

            pid_t result =
                waitpid(pid,
                        &status,
                        WNOHANG);

            if (result == pid)
                break;

            if (timeout_sec > 0 &&
                (time(NULL) - start) >= timeout_sec) {

                kill(pid, SIGKILL);

                waitpid(pid, &status, 0);

                logmsg("timeout exceeded");

                return 124;
            }

            usleep(100000);
        }

        close(pipefd[0]);

        if (verbose && total > 0)
            logmsg("%s", output);

        int exit_code = 125;

        if (WIFEXITED(status))
            exit_code = WEXITSTATUS(status);

        if (success_match &&
            regex_match(success_match,
                        output))
            return 0;

        if (exit_code_matches(exit_code,
                              success_exit_codes,
                              success_exit_count))
            return 0;

        bool should_retry = false;

        if (retry_match &&
            regex_match(retry_match,
                        output))
            should_retry = true;

        if (exit_code_matches(exit_code,
                              retry_exit_codes,
                              retry_exit_count))
            should_retry = true;

        if (should_retry &&
            attempt++ < retries) {

            sleep(interval_sec);

            continue;
        }

        return exit_code;
    }
}