#ifndef UTILS_H
#define UTILS_H 1

void daemonize_enable(void);

int daemonize_with_pidfile(char const * const pidfile);

int daemonize_shutdown(char const * const pidfile);

#endif