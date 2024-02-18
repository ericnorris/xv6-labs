#include "types.h"
#include "riscv.h"
#include "param.h"
#include "defs.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "sysinfo.h"
#include <limits.h>

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;


  argint(0, &n);
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

int
sys_pgaccess(void)
{
  // virtual address to check
  uint64 va;

  // number of pages from va to check
  int num_pages;

  // results bitmask; one bit per page where a set bit equals = accessed
  uint64 bitmask_addr;

  argaddr(0, &va);
  argint(1, &num_pages);
  argaddr(2, &bitmask_addr);

  // we can't check more pages than the number of bits in an int
  if (num_pages > sizeof(int) * CHAR_BIT) {
    return -2;
  }

  // the current process
  struct proc *proc = myproc();

  // the pte_t entry for the given virtual address
  pte_t *pte_ptr = walk(proc->pagetable, va, 0);

  // we can't check past the end of the page table
  if ((pte_ptr - proc->pagetable) + num_pages > 512) {
    return -3;
  }

  int bitmask = 0;

  for (int i = 0; i < num_pages; i++) {
    if (!(pte_ptr[i] & PTE_A)) {
      continue;
    }

    // clear the accessed flag so that we can detect if a page has been
    // accessed since the last pgaccess() call
    pte_ptr[i] &= ~(PTE_A);

    // set the bit for this page to indicate it has been accessed
    bitmask |= 1 << i;
  }

  return copyout(proc->pagetable, bitmask_addr, (char *)&bitmask,
                 sizeof(bitmask));
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  int mask;

  argint(0, &mask);

  myproc()->trace_mask = mask;

  return 0;
}

uint64
sys_sysinfo(void)
{
  // user pointer to struct sysinfo
  uint64 struct_sysinfo_addr;

  // our copy of struct sysinfo
  struct sysinfo s = {.freemem = kgetfreemem(), .nproc = proccount()};

  argaddr(0, &struct_sysinfo_addr);

  return copyout(myproc()->pagetable, struct_sysinfo_addr, (char *)&s,
                 sizeof(s));
}
