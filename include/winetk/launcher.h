#ifndef WINETK_LAUNCHER_H
#define WINETK_LAUNCHER_H
#include <stdbool.h>
#include <sys/types.h>

struct wt_launcher { pid_t pid; bool exited; int status; };
struct wt_launch_geometry { unsigned width, height, desktop_width, desktop_height; };
/* Starts the supplied Wine loader with no desktop display environment. */
int wt_launcher_start(struct wt_launcher *child, const char *wine,
                      const char *prefix, const char *socket_path,
                      const char *log, const char *cwd, unsigned fps,
                      const struct wt_launch_geometry *geometry, char *const argv[]);
int wt_launcher_poll(struct wt_launcher *child);
void wt_launcher_close(struct wt_launcher *child);
#endif
