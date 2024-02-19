#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char *argv[])
{
  int pipe_fds[2];

  if (pipe(pipe_fds) < 0) {
    fprintf(2, "error: could not open pipe\n");
    exit(1);
  }

  int pipe_fd_r = pipe_fds[0];
  int pipe_fd_w = pipe_fds[1];

  int child_pid = fork();

  if (child_pid < 0) {
    fprintf(2, "error: could not fork\n");
    exit(1);
  }

  // we need the current PID regardless of if we're the parent or the child
  int my_pid = getpid();

  // ...and we need a buffer large enough for the single byte we're going to read
  char buf[1];

  if (child_pid > 0) {
    // we're in the parent process

    // (1) write
    printf("%d: sending ping\n", my_pid);

    if (write(pipe_fd_w, "\xFF", 1) < 0) {
        fprintf(2, "error: couldn't send ping to pipe %d\n", pipe_fd_w);
        exit(1);
    }

    // (4) read
    if (read(pipe_fd_r, buf, 1) < 0) {
        fprintf(2, "error: couldn't read pong from pipe %d\n", pipe_fd_r);
        exit(1);
    }

    printf("%d: received pong\n", my_pid);

    wait(0);
  } else {
    // we're in the child process

    // (2) read
    if (read(pipe_fd_r, buf, 1) < 0) {
        fprintf(2, "error: couldn't read ping from pipe %d\n", pipe_fd_r);
        exit(1);
    }

    printf("%d: received ping\n", my_pid);

    // (3) write
    printf("%d: sending pong\n", my_pid);

    if (write(pipe_fd_w, "\xFF", 1) < 0) {
        fprintf(2, "error: couldn't send pong to pipe %d\n", pipe_fd_w);
        exit(1);
    }
  }

  exit(0);
}
