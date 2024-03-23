#include "types.h"
#include "defs.h"

struct vm_area {
  // The starting address within the process's virtual memory address space. A value of 0 means
  // this is not in use. Guaranteed to be page-aligned.
  uint64 vm_start;

  // The first byte after the end address within the virtual memory address space. May not be
  // page-aligned.
  uint64 vm_end;

  // Permissions, see fcntl.h.
  uint64 vm_prot;

  // Flags, see fcntl.h
  uint64 vm_flags;

  // List of VMAs for the process, sorted by vm_start.
  struct vm_area *vm_next;

  // The file this VMA is mapping.
  struct file *vm_file;

  // The offset within the file, in multiples of PGSIZE.
  uint vm_file_offset;

  // Since we're using a statically-allocated array of vm_area structs, this indicates if the
  // struct is "in use".
  uint used;
};
