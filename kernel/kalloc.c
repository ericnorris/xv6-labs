// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void kunchecked_free(uint cpu_core, void *pa);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct cpu_mem {
  struct spinlock lock;
  struct run *freelist;
  uint64 free_count;
} kmem[NCPU] = {};

uint page_refcounts[PHYSTOP / PGSIZE] = {};

void
kinit()
{
  char lock_name[16];

  for (int i = 0; i < NCPU; i++) {
    snprintf(lock_name, sizeof(lock_name), "kmem_%d", i);
    initlock(&kmem[i].lock, lock_name);
  }

  for (uint64 p = PGROUNDUP((uint64)end); p + PGSIZE <= PHYSTOP; p += PGSIZE) {
    // for simplicity's sake, give all free pages to the first CPU
    kunchecked_free(0, (void *)p);
  }
}

// An "unchecked" free. It won't check alignment or refcounts, nor will it wipe the memory.
void
kunchecked_free(uint cpu_core, void *pa)
{
  // From now on we'll treat the entire page as a (incredibly wasteful) struct.
  struct run *r = (struct run *)pa;

  struct cpu_mem *cpu_mem = &kmem[cpu_core];

  acquire(&cpu_mem->lock);

  r->next = cpu_mem->freelist;

  cpu_mem->freelist = r;
  cpu_mem->free_count++;

  release(&cpu_mem->lock);
}

// Free the page of physical memory pointed at by pa, which normally should have been returned by a
// call to kalloc().
void
kfree(void *pa)
{
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP) {
    panic("kfree");
  }

  // Decrement the refcount, as the caller has indicated they no longer need it.
  uint new_refcount = __sync_sub_and_fetch(&page_refcounts[(uint64)pa / PGSIZE], 1);

  // Don't actually free the page, since other processes still have references to it.
  if (new_refcount > 0) {
    return;
  }

  // At this point, we know we can free the page. Fill it with junk to catch any dangling references
  // to it.
  memset(pa, '\xD', PGSIZE);

  push_off();

  kunchecked_free(cpuid(), pa);

  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  push_off();

  int my_cpuid = cpuid();
  int i        = my_cpuid;

  do {
    struct cpu_mem *cpu_mem = &kmem[i];

    acquire(&cpu_mem->lock);

    struct run *freelist = cpu_mem->freelist;

    if (!freelist) {
      release(&cpu_mem->lock);

      // try the next CPU
      i = (i + 1 == NCPU) ? 0 : i + 1;

      continue;
    }

    cpu_mem->freelist = freelist->next;
    cpu_mem->free_count--;

    page_refcounts[(uint64)freelist / PGSIZE] = 1;

    release(&cpu_mem->lock);
    pop_off();

    // Fill with junk.
    memset((char *)freelist, 5, PGSIZE);

    return (void *)freelist;
  } while (i != my_cpuid);

  pop_off();

  // no CPUs had free memory
  return 0;
}

// Makes a copy of the given page if other pagetables have a reference to it, or returns the same
// page if
void *
kcopyonwrite(const void *pa)
{
  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP) {
    panic("kcopyonwrite");
  }

  // Decrement the refcount, since we'll either use this page or make a copy.
  uint new_refcount = __sync_sub_and_fetch(&page_refcounts[(uint64)pa / PGSIZE], 1);

  // If there are no more references, we can reuse the page.
  if (new_refcount == 0) {
    // This is probably safe? No one else has a reference, so why would there be a race condition
    // when setting the refcount for this page?
    page_refcounts[(uint64)pa / PGSIZE] = 1;

    return (void *)pa;
  }

  // We must allocate a new page, since there are still references to it.
  void *new_pa = kalloc();

  if (new_pa == 0) {
    return new_pa;
  }

  // Copy the contents of the old page to the new page before returning the new page.
  memmove(new_pa, pa, PGSIZE);

  return new_pa;
}

uint64
kgetfreemem(void)
{
  uint64 total = 0;

  for (int i = 0; i < NCPU; i++) {
    total += kmem[i].free_count;
  }

  return total * 4096;
}

void
kincrementrefcount(void *pa)
{
  __sync_fetch_and_add(&page_refcounts[(uint64)pa / PGSIZE], 1);
}
