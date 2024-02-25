// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void kunchecked_free(void *pa);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  uint64 free_count;
} kmem;

uint page_refcounts[PHYSTOP / PGSIZE] = {};

void
kinit()
{
  initlock(&kmem.lock, "kmem");

  for (uint64 p = PGROUNDUP((uint64)end); p + PGSIZE <= PHYSTOP; p += PGSIZE) {
    kunchecked_free((void *)p);
  }
}

// An "unchecked" free. It won't check alignment or refcounts, nor will it wipe the memory.
void
kunchecked_free(void *pa)
{
  // From now on we'll treat the entire page as a (incredibly wasteful) struct.
  struct run *r = (struct run *)pa;

  acquire(&kmem.lock);

  r->next       = kmem.freelist;
  kmem.freelist = r;
  kmem.free_count++;

  release(&kmem.lock);
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

  kunchecked_free(pa);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  acquire(&kmem.lock);

  struct run *r = kmem.freelist;

  if (!r) {
    // There is no free memory, so return early.
    release(&kmem.lock);
    return r;
  }

  kmem.freelist = r->next;
  kmem.free_count--;
  page_refcounts[(uint64)r / PGSIZE] = 1;

  release(&kmem.lock);

  // Fill with junk.
  memset((char *)r, 5, PGSIZE);

  return (void*)r;
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
  return kmem.free_count * 4096;
}

void
kincrementrefcount(void *pa)
{
  __sync_fetch_and_add(&page_refcounts[(uint64)pa / PGSIZE], 1);
}
