#define _POSIX_C_SOURCE 200809L
#include "winetk/launcher.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

int wt_launcher_start(struct wt_launcher *child, const char *wine,
                      const char *prefix, const char *socket_path,
                      const char *log, const char *cwd, unsigned fps,
                      const struct wt_launch_geometry *geometry, char *const argv[])
{
    size_t count=0;
    int fd, nullfd, saved;
    struct stat info;
    char **args;
    char width[16], height[16];
    if (geometry) {
        snprintf(width,sizeof(width),"%u",geometry->width);
        snprintf(height,sizeof(height),"%u",geometry->height);
    }
    while (argv[count]) ++count;
    args=calloc(count+2,sizeof(*args));
    if (!args) return -1;
    args[0]=(char *)wine;
    for (size_t i=0;i<count;++i) {
        args[i+1]=argv[i];
        if (geometry && !strcmp(argv[i],"@WINETK_WIDTH@")) args[i+1]=width;
        if (geometry && !strcmp(argv[i],"@WINETK_HEIGHT@")) args[i+1]=height;
    }
    fd=open(log,O_WRONLY|O_CREAT|O_APPEND|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK,0600);
    if (fd<0) { free(args); return -1; }
    if (fstat(fd,&info)<0 || !S_ISREG(info.st_mode)) {
        close(fd); free(args); errno=EINVAL; return -1;
    }
    nullfd=open("/dev/null",O_RDONLY|O_CLOEXEC);
    if (nullfd<0) { saved=errno; close(fd); free(args); errno=saved; return -1; }
    child->pid=fork();
    if (child->pid==0) {
        char rate[16];
        if (setpgid(0,0)<0 || dup2(nullfd,STDIN_FILENO)<0 ||
            dup2(fd,STDOUT_FILENO)<0 || dup2(fd,STDERR_FILENO)<0) _exit(126);
        close(fd); close(nullfd);
        if (cwd && chdir(cwd)<0) { perror("winetk: Wine working directory"); _exit(126); }
        unsetenv("DISPLAY"); unsetenv("WAYLAND_DISPLAY");
        snprintf(rate,sizeof(rate),"%u",fps);
        if (geometry) {
            char desktop[40];
            snprintf(desktop,sizeof(desktop),"%ux%u",geometry->desktop_width,geometry->desktop_height);
            if (setenv("WINETK_DESKTOP_SIZE",desktop,1)<0) _exit(126);
        }
        if (setenv("WINEPREFIX",prefix,1)<0 || setenv("WINETK_SOCKET",socket_path,1)<0 ||
            setenv("WINETK_FPS",rate,1)<0 || setenv("WINEDLLOVERRIDES","winemenubuilder.exe=d",1)<0) _exit(126);
        execv(wine,args);
        perror("winetk: exec Wine");
        _exit(127);
    }
    saved=errno;
    close(fd); close(nullfd); free(args);
    if (child->pid<0) { child->pid=0; errno=saved; return -1; }
    (void)setpgid(child->pid,child->pid);
    return 0;
}

int wt_launcher_poll(struct wt_launcher *child)
{
    pid_t result;
    if (!child->pid || child->exited) return child->exited;
    do result=waitpid(child->pid,&child->status,WNOHANG); while (result<0 && errno==EINTR);
    if (result==child->pid) child->exited=true;
    return result<0 ? -1 : child->exited;
}

void wt_launcher_close(struct wt_launcher *child)
{
    if (!child->pid || child->exited) return;
    /* Only our own process group; never signal a user's other Wine sessions. */
    (void)kill(-child->pid,SIGTERM);
    for (unsigned i=0;i<50 && !child->exited;++i) {
        struct timespec delay={.tv_nsec=10000000};
        if (wt_launcher_poll(child)!=0) break;
        nanosleep(&delay,NULL);
    }
    if (!child->exited) {
        (void)kill(-child->pid,SIGKILL);
        while (waitpid(child->pid,&child->status,0)<0 && errno==EINTR) {}
        child->exited=true;
    }
}
