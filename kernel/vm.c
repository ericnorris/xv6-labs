#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "proc.h"
#include "fs.h"
#include "vm.h"
#include "fcntl.h"
#include "file.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// A statically-allocated array of virtual memory areas for processes to use.
struct vm_area vmas[NPROC] = {0};

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PCI-E ECAM (configuration space), for pci.c
  kvmmap(kpgtbl, 0x30000000L, 0x30000000L, 0x10000000, PTE_R | PTE_W);

  // pci.c maps the e1000's registers here.
  kvmmap(kpgtbl, 0x40000000L, 0x40000000L, 0x20000, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);

  return kpgtbl;
}

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    panic("mappages: size");

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0) {
      printf("va=%p pte=%p\n", a, *pte);
      panic("uvmunmap: not mapped");
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// Return the address of the PTE in page table p that corresponds to the virtual address given. If
// the PTE has the PTE_COW bit set, the page is copied and the PTE is remapped to a new physical
// page.
pte_t *
uvmwalkcow(pagetable_t pagetable, uint64 va, int *cow_result)
{
  if ((va % PGSIZE) != 0) {
    panic("uvmwalkcow: va not page-aligned");
  }

  pte_t *pte = walk(pagetable, va, 0);

  if (pte == 0) {
    return pte;
  }

  uint flags = PTE_FLAGS(*pte);

  if (!(flags & PTE_COW)) {
    return pte;
  }

  if (cow_result) {
    *cow_result = 1;
  }

  uint64 old_pa = PTE2PA(*pte);
  uint64 new_pa = (uint64)kcopyonwrite((const void *)old_pa);

  if (new_pa == 0) {
    return 0;
  }

  // clear the COW bit and make the page writeable since we've allocated a new page
  flags &= ~PTE_COW;
  flags |= PTE_W;

  // remap the PTE with a new physical address and writable flags
  *pte = PA2PTE(new_pa) | flags;

  return pte;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  uint64 i;

  for (i = 0; i < sz; i += PGSIZE) {
    pte_t *pte;

    if ((pte = walk(old, i, 0)) == 0) {
      panic("uvmcopy: pte should exist");
    }

    if ((*pte & PTE_V) == 0) {
      panic("uvmcopy: page not present");
    }

    uint64 pa  = PTE2PA(*pte);
    uint flags = PTE_FLAGS(*pte);

    if (flags & PTE_W) {
      // clear the PTE_W bit to trap any writes to this page
      flags &= ~PTE_W;

      // set the PTE_COW bit so we can know to allocate a new page
      flags |= PTE_COW;

      // overwrite the parent's page table entry with the modified flags
      *pte = PA2PTE(pa) | flags;
    }

    // map the same phsyical page with the same flags in the child process - if the page was
    // originally writeable, it will now have the COW bit set and we will allocate a new writeable
    // page on demand
    if (mappages(new, i, PGSIZE, pa, flags) != 0) {
      goto err;
    }

    kincrementrefcount((void *)pa);
  }

  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);

    if (va0 >= MAXVA) {
      return -1;
    }

    pte = uvmwalkcow(pagetable, va0, 0);

    if (pte == 0 || (*pte & PTE_V) == 0 || (*pte & PTE_U) == 0) {
      return -1;
    }

    pa0 = PTE2PA(*pte);

    if ((*pte & PTE_W) == 0) {
      return -1;
    }

    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

struct vm_area *
vma_alloc()
{
  for (int i = 0; i < NELEM(vmas); i++) {
    if (__sync_bool_compare_and_swap(&vmas[i].used, 0, 1)) {
      return &vmas[i];
    }
  }

  panic("vma_alloc: no free vmas\n");
}

// Find a vm_area that contains the address within the linked list. If **prev is non-null, it will
// keep track of the previous linked list node.
struct vm_area *
vma_find(struct vm_area *list, uint64 addr, struct vm_area **prev)
{
  for (struct vm_area *vma = list; vma != 0; vma = vma->vm_next) {
    if (addr >= vma->vm_start && addr < vma->vm_end) {
      return vma;
    }

    if (prev) {
      *prev = vma;
    }
  }

  return 0;
}

// Map len bytes of the file fd in the process's address space.
uint64
mmap(struct proc *p, size_t len, int prot, int flags, int fd, off_t offset)
{
  // Ensure the proc is not trying to map a read-only file with writable permissions if it expects
  // writes to be shared with the underlying file.
  if (p->ofile[fd]->writable == 0 && (prot & PROT_WRITE) && (flags & MAP_SHARED)) {
    return ~0;
  }

  // Ensure the proc is not trying to map a non-readable file as readable.
  if (p->ofile[fd]->readable == 0 && (prot & PROT_READ)) {
    return ~0;
  }

  // Offset must be page-aligned.
  if (offset % PGSIZE != 0) {
    return ~0;
  }

  struct vm_area *vma = vma_alloc();

  // Work backwards from the last VMA for this proc, or the top of the address space.
  uint64 max_va = p->vma_list ? p->vma_list->vm_start : USYSCALL;

  vma->vm_start = PGROUNDDOWN(max_va - len);
  vma->vm_end   = vma->vm_start + len;

  vma->vm_prot        = prot;
  vma->vm_flags       = flags;
  vma->vm_file        = filedup(p->ofile[fd]);
  vma->vm_file_offset = offset;
  vma->vm_next        = p->vma_list;

  p->vma_list = vma;

  return vma->vm_start;
}

// Copy the mmap'd vma_area structs from process *p to *np.
void
mmap_copy(struct proc *p, struct proc *np)
{
  for (struct vm_area *vma = p->vma_list; vma; vma = vma->vm_next) {
    struct vm_area *copy = vma_alloc();

    *copy = *vma;

    copy->vm_next = np->vma_list;
    np->vma_list  = copy;

    filedup(copy->vm_file);
  }
}

// "Free" a given vm_area struct by writing any changes to disk if it was mappped with MAP_SHARED,
// and by unmapping the vm_area from the process. Returns the next vm_area struct in the list.
struct vm_area *
vma_free(struct proc *p, struct vm_area *prev, struct vm_area *vma)
{
  uint offset     = vma->vm_file_offset;
  uint bytes_left = vma->vm_end - vma->vm_start;

  for (uint64 page = vma->vm_start; page < vma->vm_end; page += PGSIZE) {
    pte_t *pte = walk(p->pagetable, page, 0);

    // if we haven't yet mapped the page, there's nothing to do here
    if (pte == 0 || (PTE_FLAGS(*pte) & PTE_V) == 0) {
      continue;
    }

    uint length = bytes_left > PGSIZE ? PGSIZE : bytes_left;

    if ((vma->vm_flags & MAP_SHARED) && (PTE_FLAGS(*pte) & PTE_D)) {
      begin_op();
      ilock(vma->vm_file->ip);

      writei(vma->vm_file->ip, 0, PTE2PA(*pte), offset, length);

      iunlock(vma->vm_file->ip);
      end_op();
    }

    offset     += PGSIZE;
    bytes_left -= length;

    uvmunmap(p->pagetable, page, 1, 1);
  }

  fileclose(vma->vm_file);

  struct vm_area *next = vma->vm_next;

  if (prev) {
    prev->vm_next = next;
  } else {
    p->vma_list = next;
  }

  // "Free" the vm_area
  *vma = (const struct vm_area){0};

  return next;
}

// Unmap any vm_area structs in the range specified by [addr, addr + len].
int
munmap(struct proc *p, uint64 addr, size_t len)
{
  uint64 unmap_start = PGROUNDDOWN((uint64)addr);
  uint64 unmap_end   = PGROUNDUP(unmap_start + len);

  struct vm_area *prev = 0;
  struct vm_area *vma  = vma_find(p->vma_list, unmap_start, &prev);

  if (vma == 0) {
    // "If there are no mappings in the specified address range, then munmap() has no effect.""
    return 0;
  }

  // We may have unmap_start > vma_start->vm_start, which means we need to split the vma into:
  //
  //   - vma [start, unmap_start]
  //   - new [unmap_start, end]
  if (unmap_start > vma->vm_start) {
    struct vm_area *new = vma_alloc();

    new->vm_start       = unmap_start;
    new->vm_end         = vma->vm_end;
    new->vm_prot        = vma->vm_prot;
    new->vm_flags       = vma->vm_flags;
    new->vm_next        = vma->vm_next;
    new->vm_file        = filedup(vma->vm_file);
    new->vm_file_offset = vma->vm_file_offset + (unmap_start - vma->vm_start);

    vma->vm_end  = unmap_start;
    vma->vm_next = new;

    prev = vma;
    vma  = new;
  }

  do {
    // We may have unmap_end < end, which means we need to split the vma (potentially for the second
    // time) into:
    //
    //   - vma [start, unmap_end]
    //   - new [unmap_end, end]
    if (unmap_end < vma->vm_end) {
      struct vm_area *new = vma_alloc();

      new->vm_start       = unmap_end;
      new->vm_end         = vma->vm_end;
      new->vm_prot        = vma->vm_prot;
      new->vm_flags       = vma->vm_flags;
      new->vm_next        = vma->vm_next;
      new->vm_file        = filedup(vma->vm_file);
      new->vm_file_offset = vma->vm_file_offset + (unmap_end - vma->vm_start);

      vma->vm_end  = unmap_end;
      vma->vm_next = new;
    }

    vma = vma_free(p, prev, vma);
  } while (vma && (unmap_end > vma->vm_start));

  return 0;
}

// Unmap all vm_area structs for the process *p.
int
munmap_all(struct proc *p)
{
  for (struct vm_area *vma = p->vma_list; vma; vma = vma_free(p, 0, vma)) {}

  return 0;
}

// If the address va is within the process's mmap'd address space, allocates a physical page and
// copies the file contents for the vm_area struct that va intersects.
int
mmap_page_fault_handler(struct proc *p, uint64 va)
{
  if ((va % PGSIZE) != 0) {
    panic("mmap_page_fault_handler: va not page-aligned");
  }

  struct vm_area *vma = vma_find(p->vma_list, va, 0);

  if (vma == 0) {
    return 0;
  }

  uint64 pa = (uint64)kalloc();

  if (pa == 0) {
    panic("mmap_page_fault_handler: no free mem\n");
  }

  memset((void *)pa, 0, PGSIZE);

  uint offset = vma->vm_file_offset + (va - vma->vm_start);
  uint length = vma->vm_end - offset > PGSIZE ? PGSIZE : vma->vm_end - offset;

  ilock(vma->vm_file->ip);
  readi(vma->vm_file->ip, 0, pa, offset, length);
  iunlock(vma->vm_file->ip);

  int perm = ((vma->vm_prot && PROT_READ) * PTE_R) | ((vma->vm_prot && PROT_WRITE) * PTE_W) |
             ((vma->vm_prot && PROT_EXEC) * PTE_X) | PTE_U;

  if (mappages(p->pagetable, va, PGSIZE, pa, perm) < 0) {
    return -1;
  }

  return 1;
}

void
vmprint_rec(pagetable_t pagetable, int indent)
{
  // there are 2^9 = 512 PTEs in a page table
  for (int i = 0; i < 512; i++) {
    pte_t pte = pagetable[i];
    uint64 pa = PTE2PA(pte);

    if (!(pte & PTE_V)) {
      continue;
    }

    for (int j = 0; j < indent; j++) {
      printf(" ..");
    }

    printf("%d: pte %p pa %p flags %x\n", i, pte, pa, PTE_FLAGS(pte));

    if ((pte & (PTE_R | PTE_W | PTE_X | PTE_COW)) == 0) {
      // this PTE points to a lower-level page table
      vmprint_rec((pagetable_t)pa, indent + 1);
    }
  }
}

// Recursively print page-table pages.
void
vmprint(pagetable_t pagetable)
{
  printf("page table %p\n", pagetable);

  vmprint_rec(pagetable, 1);
}
