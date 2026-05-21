#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <netdb.h>
#include <regex.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define VERSION "0.0.1"

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

static void logmsg(const char *fmt, ...) {
    if (quiet) return;

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
"        Example:\n"
"          delayme -s 2 ./httpscheck\n"
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
"--relative[=MODE]\n"
"--absolute[=MODE]\n"
"\n"
"--success-match STR\n"
"--retry-match STR\n"
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
    if (sock < 0) return false;

    struct hostent *he = gethostbyname(host);
    if (!he) {
        close(sock);
        return false;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);

    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    int result = connect(sock, (struct sockaddr*)&addr, sizeof(addr));

    close(sock);

    return result == 0;
}

static bool regex_match(const char *pattern, const char *text) {
    regex_t regex;

    if (regcomp(&regex, pattern, REG_EXTENDED | REG_NOSUB) != 0)
        return false;

    bool matched = regexec(&regex, text, 0, NULL, 0) == 0;

    regfree(&regex);

    return matched;
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
        {"relative", optional_argument, 0, 4},
        {"absolute", optional_argument, 0, 5},
        {"success-match", required_argument, 0, 6},
        {"retry-match", required_argument, 0, 7},
        {0,0,0,0}
    };

    while ((opt = getopt_long(argc, argv, "s:t:r:i:qvh", long_opts, NULL)) != -1) {
        switch (opt) {
            case 's': sleep_sec = atoi(optarg); break;
            case 't': timeout_sec = atoi(optarg); break;
            case 'r': retries = atoi(optarg); break;
            case 'i': interval_sec = atoi(optarg); break;
            case 'q': quiet = true; break;
            case 'v': verbose = true; break;
            case 'h': usage(); return 0;

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
        }
    }

    if (optind >= argc) {
        usage();
        return 125;
    }

    if (sleep_sec > 0)
        sleep(sleep_sec);

    while (ready_file && !file_exists(ready_file))
        sleep(interval_sec);

    while (wait_port && !port_open(wait_port))
        sleep(interval_sec);

    char *cmd = argv[optind];

    int attempt = 0;

    while (1) {
        int pipefd[2];
        pipe(pipefd);

        pid_t pid = fork();

        if (pid == 0) {
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);

            close(pipefd[0]);

            execvp(cmd, &argv[optind]);

            perror("execvp");
            exit(125);
        }

        close(pipefd[1]);

        char output[8192] = {0};

        read(pipefd[0], output, sizeof(output)-1);

        int status;
        waitpid(pid, &status, 0);

        if (verbose)
            logmsg("%s", output);

        if (success_match && regex_match(success_match, output))
            return 0;

        if (retry_match && regex_match(retry_match, output)) {
            if (attempt++ < retries) {
                sleep(interval_sec);
                continue;
            }
        }

        if (WIFEXITED(status))
            return WEXITSTATUS(status);

        return 125;
    }
}