[![Build](https://github.com/LostOnTheLine/DelayMe/actions/workflows/build.yml/badge.svg)](https://github.com/LostOnTheLine/DelayMe/actions)
[![License](https://img.shields.io/badge/license-PolyForm%20Noncommercial-blue)]()
[![Version](https://img.shields.io/github/v/release/LostOnTheLine/DelayMe)]()

# DelayMe
A tiny Linux utility for delayed & conditional execution of binaries. Originally created to solve orchestration limitations in shell-less Docker healthcheck environments, but useful anywhere precise startup timing & execution control is needed.

Designed for:
- Docker healthchecks
- distroless containers
- scratch containers
- Synology Docker
- environments without shell/curl/wget
- healthchecks with custom success conditions

## Features

- startup delay
- timeout handling
- retries
- retry intervals
- wait for TCP port
- wait for file existence
- relative executable resolution
- stdout/stderr pattern matching (regex or substring)
- success exit code matching
- retry exit code matching

## Why

Many minimal containers do not include:
- `/bin/sh`
- `curl`
- `wget`
- `sleep`
- `timeout`

Docker healthchecks provide limited control over startup timing, retry behavior, & conditional readiness.

DelayMe provides a single static binary that can:
- delay execution
- retry checks
- wait for ports/files
- interpret custom success states
- run without shell dependencies

## Build

```bash
make
```

Static build:

```bash
CC=musl-gcc make
```

## Usage

```bash
delayme [options] <program> [args...]
```

## Examples

Wait 2 seconds:

```bash
delayme -s 2 ./httpscheck localhost:9000/api/system/status
```

Retry startup failures:

```bash
delayme -s 2 -r 5 -i 1 ./httpscheck localhost:9000/api/system/status
```

Wait for TCP port:

```bash
delayme --wait-port localhost:9000 ./httpscheck
```

Wait for file:

```bash
delayme --ready-file /tmp/ready ./httpscheck
```

Relative execution:

```bash
delayme --relative binary
```

Timeout:

```bash
delayme -t 10 ./httpscheck
```

Success matching:

```bash
delayme --success-exit 0,1 ./check
```

Retry matching:

```bash
delayme --retry-exit 2 ./check
```

Retry & success exit handling:

```bash
delayme -r 5 -i 2 \
        --retry-exit 2 \
        --success-exit 0 \
        ./check
```

Output matching:

```bash
delayme --success-match healthy \
        --retry-match warming \
        ./check-service
```

## Exit Codes

| Code | Meaning |
|------|---------|
| 0 | success |
| 124 | timeout |
| 125 | internal error |
| child exit | propagated child status |

## Security

DelayMe is intended for trusted/local execution environments.

It is not designed as a sandbox or privilege boundary.

## License

This project is licensed for personal, hobby, nonprofit, & educational use under the PolyForm Noncommercial License 1.0.0.

Personal, hobby, educational, & nonprofit use are permitted.

Commercial use, including internal business use, requires a custom license & written permission from the author.

For commercial licensing inquiries, contact via GitHub.

## Contributing

By contributing to this project, you agree that your contributions are licensed under the same terms as the project license.