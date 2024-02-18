#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "kernel/param.h"
#include "user/user.h"

int
read_line(int fd, char *buf, int buf_len)
{
  int bytes_read = 0;

  int last_read;
  char last_char;

  while ((last_read = read(fd, &last_char, sizeof last_char)) > 0) {
    bytes_read++;

    if (bytes_read > buf_len) {
      fprintf(2, "error: line too long");
      return -1;
    }

    if (last_char == '\n') {
      *buf++ = '\0';

      return bytes_read;
    }

    *buf++ = last_char;
  }

  *buf = '\0';

  if (last_read < 0) {
    fprintf(2, "error: could not read fd '%d'", fd);
    return -1;
  }

  // EOF
  return 0;
}

int
main(int argc, char *argv[])
{
  if (argc < 2) {
    fprintf(2, "Usage: xargs <command> [arguments]\n");
    exit(-1);
  }

  char buf[512];

  // the arguments we will pass to the exec() call in the child process
  char *child_argv[MAXARG] = {0};

  // the number of arguments we are passing to the child process
  int child_argc;

  // first, use all but the first argument to this process as arguments for
  // the child process
  for (child_argc = 0; child_argc < argc - 1; child_argc++) {
    child_argv[child_argc] = argv[child_argc + 1];
  }

  // the starting point for reading arguments from stdin into child_argv
  int child_argv_start = child_argc;

  int readline_rv;

  while ((readline_rv = read_line(0, buf, sizeof buf)) >= 0) {
    // reset the child_argc count to the arguments passed to xargs
    child_argc = child_argv_start;

    // reset the last child_argv to NULL
    child_argv[child_argc] = 0;

    for (char *c = buf; *c != '\0'; c++) {
      // skip whitespace until we've found the start of the argument
      if (*c == ' ' && child_argv[child_argc] == 0) {
        continue;
      }

      if (*c != ' ' && child_argv[child_argc] == 0) {
        // we've found the start of the argument
        child_argv[child_argc] = c;

        continue;
      }

      if (*c == ' ') {
        // we've found the end of the argument
        *c = '\0';
        child_argc++;

        continue;
      }
    }

    // we saw at least one non-whitespace character, but did not see a ' ', so
    // terminate the current argument
    if (child_argv[child_argc] != 0) {
      child_argc++;
    }

    // signify the end of the arguments
    child_argv[child_argc] = 0;

    int child_pid = fork();

    if (child_pid < 0) {
      fprintf(2, "error: could not fork");
      exit(-1);
    }

    if (child_pid == 0) {
      if (exec(argv[1], child_argv) < 0) {
        fprintf(2, "error: could not exec");
        exit(-1);
      }
    } else {
      wait(0);
    }

    // if we saw an EOF, there will be no more lines
    if (readline_rv == 0) {
      break;
    }
  }

  exit(0);
}
