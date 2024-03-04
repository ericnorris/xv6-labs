#include "kernel/types.h"
#include "user/user.h"

int
sieve(int pipe_fds[2]);

int
main(int argc, char *argv[])
{
  // the eventual return value of the program
  int rv = 0;

  int pipe_fds[2];

  if (pipe(pipe_fds) < 0) {
    fprintf(2, "error: could not open initial pipe\n");
    goto exit;
  }

  int child_pid = fork();

  if (child_pid < 0) {
    rv = 1;
    fprintf(2, "error: could not fork initial child process\n");
    goto teardown;
  }

  if (child_pid == 0) {
    // we're in the child
    exit(sieve(pipe_fds));
  }

  // since we're in the parent process, we no longer need the read half of the
  // pipe
  if (close(pipe_fds[0]) < 0) {
    rv = 1;
    fprintf(2, "error: could not close read half of initial pipe\n");
    goto teardown;
  }

  int write_fd = pipe_fds[1];

  for (int i = 2; i <= 35; i++) {
    if (write(write_fd, &i, sizeof(i)) < 0) {
      rv = 1;
      fprintf(2, "error: could not write integer to child process");
      goto teardown;
    }
  }

teardown:
  close(pipe_fds[0]);
  close(pipe_fds[1]);

  if (child_pid) {
    wait(0);
  }

exit:
  exit(rv);
}

int
sieve(int pipe_fds[2])
{
init:
  // the read pipe from our parent
  int read_fd = pipe_fds[0];

  // our prime, initially unknown
  int p = 0;

  // the number we've read from our parent
  int n;

  // the most recent multiple of p that is greater than or equal to the last n
  int high_watermark;

  // the result of the last read() call
  int bytes_read;

  // the write pipe to our child, initially unset
  int write_fd = 0;

  // the eventual return value of this function
  int rv = 0;

  // first, close the write pipe from our parent, since we won't need it
  if (close(pipe_fds[1])) {
    rv = 1;
    fprintf(2, "error: could not close write half of pipe in child process");
    goto teardown;
  }

  // then begin the loop
  while ((bytes_read = read(read_fd, &n, sizeof(n)))) {
    if (p == 0) {
      // first iteration, we now know our prime
      p = n;

      // and the initial high_watermark
      high_watermark = p;

      printf("prime %d\n", p);

      continue;
    }

    // increment the high_watermark until it is greater than or equal to n...
    for (; high_watermark < n; high_watermark += p) {}

    // ...and if the high_watermark equals n, it is divisible by p
    if (high_watermark == n) {
      continue;
    }

    // since it is not divisible by p, the number may be prime and should be
    // passed on to the right neighbor

    if (write_fd == 0) {
      // we've yet to fork, so create a new pipe - note that we're reusing the
      // pipe_fds local variable
      if (pipe(pipe_fds) < 0) {
        rv = 1;
        fprintf(2, "error: could not create pipe in child process");
        goto teardown;
      }

      int child_pid = fork();

      if (child_pid < 0) {
        rv = 1;
        fprintf(2, "error: could not fork in child process");
        goto teardown;
      }

      if (child_pid == 0) {
        // we're in the child, so we'll go to the init label - note that this
        // works as if we're just calling the sieve() function, because we've
        // reused the pipe_fds variable
        goto init;
      }

      // we're in the parent, so we don't need the read half of the pipe
      if (close(pipe_fds[0]) < 0) {
        rv = 1;
        fprintf(2, "error: could not close read half of pipe in child process");
        goto teardown;
      }

      write_fd = pipe_fds[1];
    }

    // pass the maybe-prime into the next sieve
    if (write(write_fd, &n, sizeof(n)) < 0) {
      rv = 1;
      fprintf(2, "error: could not write to child process");
      goto teardown;
    }
  }

  if (bytes_read < 0) {
    // the last read encountered an error
    rv = 1;
    fprintf(2, "error: could not read from parent process");
    goto teardown;
  }

teardown:
  close(read_fd);

  // if write_fd is non-zero, we've forked a right neighbor and should clean it
  // up
  if (write_fd) {
    // close the write half of the pipe to tell the right neighbor to shut down
    close(write_fd);

    // wait for it to exit
    wait(0);
  }

  return rv;
}
