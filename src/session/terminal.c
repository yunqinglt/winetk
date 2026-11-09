#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include "winetk/terminal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t stopped, resized;
static const int watched[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGTSTP, SIGWINCH, SIGPIPE};

static void signal_handler(int number)
{
    if (number == SIGWINCH) resized = 1;
    else if (number != SIGPIPE) stopped = number;
}

uint64_t wt_now_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
}

int wt_terminal_open(struct wt_terminal *terminal)
{
    struct termios raw;
    struct sigaction action;
    size_t installed = 0;
    memset(terminal, 0, sizeof(*terminal));
    terminal->fd = terminal->recording_fd = -1;
    stopped = resized = 0;
    terminal->fd = open("/dev/tty", O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (terminal->fd < 0) return -1;
    if (tcgetattr(terminal->fd, &terminal->saved) < 0) goto fail;
    memset(&action, 0, sizeof(action));
    action.sa_handler = signal_handler;
    sigemptyset(&action.sa_mask);
    for (; installed < sizeof(watched) / sizeof(*watched); ++installed)
        if (sigaction(watched[installed], &action, &terminal->previous[installed]) < 0) goto fail;
    raw = terminal->saved;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(terminal->fd, TCSANOW, &raw) < 0) goto fail;
    terminal->raw = true;
    return 0;
fail:
    {
        int saved_errno = errno;
        while (installed) {
            --installed;
            sigaction(watched[installed], &terminal->previous[installed], NULL);
        }
        close(terminal->fd);
        terminal->fd = -1;
        errno = saved_errno;
        return -1;
    }
}

ssize_t wt_terminal_write(void *context, const void *data, size_t length)
{
    struct wt_terminal *terminal = context;
    const unsigned char *bytes = data;
    size_t sent = 0;
    uint64_t deadline = terminal->deadline_ms ? terminal->deadline_ms : wt_now_ms() + 1500;
    while (sent < length) {
        ssize_t n;
        if (!terminal->closing && stopped) { errno = ECANCELED; return -1; }
        if (wt_now_ms() >= deadline) { errno = ETIMEDOUT; return -1; }
        n = write(terminal->fd, bytes + sent, length - sent);
        if (n > 0) {
            terminal->bytes += (uint64_t)n;
            if (terminal->record_output && terminal->recording_fd >= 0) {
                size_t recorded = 0;
                while (recorded < (size_t)n) {
                    if ((!terminal->closing && stopped) || wt_now_ms() >= deadline) {
                        errno = stopped ? ECANCELED : ETIMEDOUT;
                        return -1;
                    }
                    ssize_t written = write(terminal->recording_fd, bytes + sent + recorded, (size_t)n - recorded);
                    if (written < 0 && errno == EINTR) continue;
                    if (written <= 0) return -1;
                    recorded += (size_t)written;
                }
            }
            sent += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) return -1;
        if (wt_now_ms() >= deadline) { errno = ETIMEDOUT; return -1; }
        {
            struct pollfd fd = {.fd = terminal->fd, .events = POLLOUT};
            int wait = (int)(deadline - wt_now_ms());
            if (wait > 50) wait = 50;
            if (wait < 0) wait = 0;
            if (poll(&fd, 1, wait) < 0 && errno != EINTR) return -1;
            if (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) { errno = EIO; return -1; }
        }
    }
    return (ssize_t)sent;
}

int wt_terminal_command(struct wt_terminal *terminal, const char *command)
{
    return wt_terminal_write(terminal, command, strlen(command)) < 0 ? -1 : 0;
}

int wt_terminal_activate(struct wt_terminal *terminal, bool pixel_mouse)
{
    /* Stages prevent an interrupted setup from popping the caller's keyboard stack. */
    if (wt_terminal_command(terminal,"\033[?25;1000;1002;1003;1004;1006;1016;2004;2026s") < 0) return -1;
    terminal->modes_saved = true;
    if (wt_terminal_command(terminal,"\033[?1049h") < 0) return -1;
    terminal->alternate_entered = true;
    if (wt_terminal_command(terminal,"\033[>31u") < 0) return -1;
    terminal->keyboard_pushed = true;
    if (wt_terminal_command(terminal,
        "\033[2J\033[H\033[?25l\033[?1000l\033[?1002l\033[?1003h"
        "\033[?1004h\033[?1006h\033[?2004h") < 0) return -1;
    terminal->active = true;
    return wt_terminal_command(terminal, pixel_mouse ? "\033[?1016h" : "\033[?1016l");
}

void wt_terminal_close(struct wt_terminal *terminal)
{
    if (terminal->fd < 0) return;
    terminal->closing = true;
    terminal->record_output = false;
    terminal->deadline_ms = wt_now_ms() + 500;
    /* CAN cancels a partial CSI. The graphics backend terminates any APC it
     * owns; an unconditional ST in ground state is a Kitty parse error. */
    (void)wt_terminal_command(terminal,"\030");
    if (terminal->alternate_entered) (void)wt_terminal_command(terminal,"\033[?2026l");
    if (terminal->keyboard_pushed) (void)wt_terminal_command(terminal,"\033[<u");
    if (terminal->alternate_entered) (void)wt_terminal_command(terminal,"\033[?1049l");
    if (terminal->modes_saved)
        (void)wt_terminal_command(terminal,"\033[?25;1000;1002;1003;1004;1006;1016;2004;2026r");
    if (terminal->raw) while (tcsetattr(terminal->fd, TCSANOW, &terminal->saved) < 0 && errno == EINTR) {}
    close(terminal->fd);
    terminal->fd = -1;
    for (size_t i = 0; i < sizeof(watched) / sizeof(*watched); ++i)
        sigaction(watched[i], &terminal->previous[i], NULL);
}

int wt_terminal_signal(void) { return stopped; }
bool wt_terminal_resized(void)
{
    sigset_t blocked, previous;
    bool result;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGWINCH);
    sigprocmask(SIG_BLOCK, &blocked, &previous);
    result = resized != 0;
    resized = 0;
    sigprocmask(SIG_SETMASK, &previous, NULL);
    return result;
}
