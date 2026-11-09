#ifndef WINETK_TERMINAL_H
#define WINETK_TERMINAL_H

#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <termios.h>

struct wt_terminal {
    int fd;
    struct termios saved;
    struct sigaction previous[7];
    bool raw, active, closing, modes_saved, alternate_entered, keyboard_pushed;
    uint64_t deadline_ms, bytes;
    int recording_fd;
    bool record_output;
};

uint64_t wt_now_ms(void);
int wt_terminal_open(struct wt_terminal *terminal);
ssize_t wt_terminal_write(void *context, const void *data, size_t length);
int wt_terminal_command(struct wt_terminal *terminal, const char *command);
int wt_terminal_activate(struct wt_terminal *terminal, bool pixel_mouse);
void wt_terminal_close(struct wt_terminal *terminal);
int wt_terminal_signal(void);
bool wt_terminal_resized(void);

#endif
