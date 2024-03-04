#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

int
is_directory(char *path)
{
  int rv = 0;
  int path_fd = 0;

  if ((path_fd = open(path, O_RDONLY)) < 0) {
    fprintf(2, "error: could not open '%s' for reading\n", path);
    return path_fd;
  }

  struct stat path_stat;

  if ((rv = fstat(path_fd, &path_stat)) < 0) {
    fprintf(2, "error: could not stat '%s'\n", path);
    goto teardown;
  }

  if (path_stat.type == T_DIR) {
    return path_fd;
  }

  rv = 0;

teardown:
  close(path_fd);

  return rv;
}

int
recurse_directory(int dir_fd, char *path, int path_len, char *name)
{
  printf("recursing into %s (path_len: %d)\n", path, path_len);
  // a buffer to hold the path to a directory entry
  char buf[512];

  // an entry in the directory
  struct dirent entry;

  // the result of the last read() call
  int bytes_read;

  // if an entry is a subdirectory, this will be an fd for that directory
  int subdir_fd;

  // if the length of (path + '/' + DIRSIZE + NULL) is larger than the buffer,
  // we can't continue, because we can't fit new path into the buffer
  if (path_len + 1 + sizeof(entry.name) + 1 > sizeof buf) {
    fprintf(2, "error: path '%s' is too long to continue searching");
    return -1;
  }

  // first copy the path to the buf
  strcpy(buf, path);

  // then add a trailing slash
  buf[path_len] = '/';

  // basename points to after the trailing slash in *path
  char *basename = buf + path_len + 1;

  while ((bytes_read = read(dir_fd, &entry, sizeof entry)) > 0) {
    // skip free directory entries
    if (entry.inum == 0) {
      continue;
    }

    // skip the current and parent directories to prevent an infinite loop
    if (strcmp(entry.name, ".") == 0 || strcmp(entry.name, "..") == 0) {
      continue;
    }

    // copy the current entry name into the buffer after the trailing slash
    memmove(basename, entry.name, sizeof entry.name);

    // it may be shorter than DIRSIZE, but we need to null-terminate it here
    // in case it isn't
    basename[sizeof entry.name] = '\0';

    // calculate the new path length with the guaranteed trailing NULL
    int new_path_len = (basename - buf) + strlen(basename);

    // we've found a match
    if (strcmp(basename, name) == 0) {
      printf("%s\n", buf);
    }

    if ((subdir_fd = is_directory(buf)) > 0) {
      // we're passing 'buf' here, which is stack-allocated; this is only safe
      // so long as 'buf'outlives the recurse_directory call
      if (recurse_directory(subdir_fd, buf, new_path_len, name) < 0) {
        close(subdir_fd);
        return -1;
      }

      if (close(subdir_fd) < 0) {
        fprintf(2, "error: could not close subdirectory fd for '%s'", buf);
        return -1;
      }
    }

    if (subdir_fd < 0) {
      return -1;
    }
  }

  return 0;
}

void
find(char *path, char *name)
{
  // check if the basename of the path matches the name, print if so
  // if it's a directory, recurse_directory

  // basename will point to the basename of the path
  char *basename = path;

  // the length of the path
  int path_len = 0;

  // if the path is a directory, dir_fd will be non-zero
  int dir_fd = is_directory(path);

  if (dir_fd < 0) {
    exit(-1);
  }

  // first we need to determine the basename and length of the path
  for (char *c = path; *c != '\0'; path_len++, c++) {
    // move basename to the most recent / character, unless it's a trailing /
    if (c[0] == '/' && c[1] != '\0') {
      basename = c;
    }
  }

  int has_trailing_slash = path[path_len - 1] == '/';

  // if it has a trailing slash, we'll remove it, since is_directory expects
  // path to not have a trailing slash
  if (has_trailing_slash) {
    path[path_len - 1] = '\0';
    path_len--;
  }

  if (strcmp(basename, name) == 0) {
    // we'll preserve the trailing slash if path matches
    printf("%s%s", path, has_trailing_slash ? "/" : "");
  }

  if (dir_fd > 0) {
    recurse_directory(dir_fd, path, path_len, name);
  }
}

int
main(int argc, char *argv[])
{
  if (argc != 3) {
    fprintf(2, "Usage: find [path] [name]\n");
    exit(1);
  }

  find(argv[1], argv[2]);

  exit(0);
}
