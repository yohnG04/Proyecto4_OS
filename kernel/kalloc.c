// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

#define NPAGE (PHYSTOP / PGSIZE)
static struct {
  struct spinlock lock;
  int refs[NPAGE];
} krefs;

static int
refindex(uint64 pa)
{
  if (pa % PGSIZE || pa < KERNBASE || pa >= PHYSTOP)
    panic("kref: address");
  return pa / PGSIZE;
}

void
krefinc(uint64 pa)
{
  acquire(&krefs.lock);
  int i = refindex(pa);
  if (krefs.refs[i] < 1)
    panic("krefinc: free");
  krefs.refs[i]++;
  release(&krefs.lock);
}

int
krefcount(uint64 pa)
{
  acquire(&krefs.lock);
  int n = krefs.refs[refindex(pa)];
  release(&krefs.lock);
  return n;
}

uint64
ksharedpages(void)
{
  uint64 n = 0;
  acquire(&krefs.lock);
  for (int i = 0; i < NPAGE; i++)
    if (krefs.refs[i] > 1)
      n++;
  release(&krefs.lock);
  return n;
}

uint64
kfreepages(void)
{
  uint64 n = 0;
  acquire(&kmem.lock);
  for (struct run *r = kmem.freelist; r; r = r->next)
    n++;
  release(&kmem.lock);
  return n;
}

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&krefs.lock, "krefs");
  // freerange() releases pages that have not previously been allocated.
  for (uint64 pa = PGROUNDUP((uint64)end); pa < PHYSTOP; pa += PGSIZE)
    krefs.refs[pa / PGSIZE] = 1;
  freerange(end, (void *)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  int i = refindex((uint64)pa);
  acquire(&krefs.lock);
  if (krefs.refs[i] < 1)
    panic("kfree: ref");
  if (--krefs.refs[i] > 0) {
    release(&krefs.lock);
    return;
  }
  release(&krefs.lock);

  memset(pa, 1, PGSIZE);
  r = (struct run *)pa;
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r) {
    acquire(&krefs.lock);
    krefs.refs[refindex((uint64)r)] = 1;
    release(&krefs.lock);
    memset((char *)r, 5, PGSIZE); // fill with junk
  }
  return (void *)r;
}
