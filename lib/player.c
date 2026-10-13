/* UniBin Linux player library: process-based playback via ffplay or aplay. */
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

static pid_t ub_audio_pid = -1;

static void ub_audio_stop(void) {
    if (ub_audio_pid > 0) {
        kill(ub_audio_pid, SIGTERM);
        while (waitpid(ub_audio_pid, NULL, 0) < 0 && errno == EINTR) { }
        ub_audio_pid = -1;
    }
}

static void ub_audio_play(const char *path) {
    ub_audio_stop();
    pid_t p = fork();
    if (p < 0) { perror("UniBin audio fork"); return; }
    if (p == 0) {
        int fd = open("/dev/null", O_WRONLY);
        if (fd >= 0) { dup2(fd, 1); dup2(fd, 2); close(fd); }
        execlp("ffplay", "ffplay", "-nodisp", "-autoexit", "-loglevel", "error", path, (char *)NULL);
        execlp("aplay", "aplay", "-q", path, (char *)NULL);
        _exit(127);
    }
    ub_audio_pid = p;
}

static void ub_audio_pause(void) { if (ub_audio_pid > 0) kill(ub_audio_pid, SIGSTOP); }
static void ub_audio_resume(void) { if (ub_audio_pid > 0) kill(ub_audio_pid, SIGCONT); }
