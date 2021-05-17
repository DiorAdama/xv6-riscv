#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

void print(pagetable_t);

/*
 * create a direct-map page table for the kernel and
 * turn on paging. called early, in supervisor mode.
 * the page allocator is already initialized.
 */
void
kvminit()
{
  kernel_pagetable = (pagetable_t) kalloc();
  memset(kernel_pagetable, 0, PGSIZE);

  // uart registers
  kvmmap(UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface 0
  kvmmap(VIRTION(0), VIRTION(0), PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface 1
  kvmmap(VIRTION(1), VIRTION(1), PGSIZE, PTE_R | PTE_W);

  // CLINT
  kvmmap(CLINT, CLINT, 0x10000, PTE_R | PTE_W);

  // PLIC
  kvmmap(PLIC, PLIC, 0x400000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap((uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
void
kvminithart()
{
  w_satp(MAKE_SATP(kernel_pagetable));
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
//   21..39 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..12 -- 12 bits of byte offset within the page.
static pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    return 0;

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
kvmmap(uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kernel_pagetable, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// translate a kernel virtual address to
// a physical address. only needed for
// addresses on the stack.
// assumes va is page aligned.
uint64
kvmpa(uint64 va)
{
  uint64 off = va % PGSIZE;
  pte_t *pte;
  uint64 pa;
  
  pte = walk(kernel_pagetable, va, 0);
  if(pte == 0)
    panic("kvmpa");
  if((*pte & PTE_V) == 0)
    panic("kvmpa");
  pa = PTE2PA(*pte);
  return pa+off;
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

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// Remove mappings from a page table. The mappings in
// the given range must exist. Optionally free the
// physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 size, int do_free)
{
  uint64 a, last;
  pte_t *pte;

  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(; a <= last; a+=PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0){
      continue;
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free){
      kfree((void*)PTE2PA(*pte));
    }
    *pte = 0;
  }
}

// create an empty user page table.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    panic("uvmcreate: out of memory");
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvminit(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("inituvm: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  a = oldsz;
  for(; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_W|PTE_X|PTE_R|PTE_U) != 0){
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

  uint64 newup = PGROUNDUP(newsz);
  if(newup < PGROUNDUP(oldsz))
    uvmunmap(pagetable, newup, oldsz - newup, 1);

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
static void
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
  uvmunmap(pagetable, 0, sz, 1);
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
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;
    if((*pte & PTE_V) == 0)
      continue;
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i, 1);
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


int load_from_file(char* file,
                   uint64 file_start_offset,
                   uint64 pa,
                   uint64 nbytes
  ){
    struct inode* ip;
    begin_op(ROOTDEV);
    if((ip = namei(file)) == 0){
      printf("in load_from_file\n");
      end_op(ROOTDEV);
      return -1;
    }
    ilock(ip);
    if(readi(ip, 0, pa, file_start_offset, nbytes) != nbytes){
      iunlockput(ip);
      end_op(ROOTDEV);
      return -1;
    }
    iunlockput(ip);
    end_op(ROOTDEV);
    ip = 0;
    return 0;
  }

int do_allocate(pagetable_t pagetable, struct proc* p, uint64 addr, uint64 scause){
  pte_t* pte;
  struct vma* vma_of_addr;
  void* pa;
  vma_of_addr = get_memory_area(p, addr);
  if ((pte = walk(pagetable, addr, 0)) == 0 || (*pte & PTE_V) == 0){

    if (vma_of_addr == 0) return ENOVMA;
    if ((scause == CAUSE_R && (PTE_R & vma_of_addr->vma_flags) == 0) || 
        (scause == CAUSE_W && (PTE_W & vma_of_addr->vma_flags) == 0) ||
        (scause == CAUSE_X && (PTE_X & vma_of_addr->vma_flags) == 0))
      return EBADPERM;

    if ((pa = kalloc()) == 0) return ENOMEM;

    if (((uint64) pa) % PGSIZE != 0) {
      kfree(pa);
      return ENOMEM;
    }
    int flags = PTE_U;
    flags |= (vma_of_addr->vma_flags & VMA_R) != 0 ? PTE_R : 0;
    flags |= (vma_of_addr->vma_flags & VMA_W) != 0 ? PTE_W : 0;
    flags |= (vma_of_addr->vma_flags & VMA_X) != 0 ? PTE_X : 0;

    if (mappages(pagetable, addr, PGSIZE, (uint64) pa, flags) != 0){
      kfree(pa);
      return EMAPFAILED;
    }

    if (vma_of_addr->file) {
      uint64 file_start_offset = vma_of_addr->file_offset + (addr - vma_of_addr->va_begin); 
      if (file_start_offset > vma_of_addr->file_offset + vma_of_addr->file_nbytes){
        return 0;
      }
      uint64 remainder = vma_of_addr->file_offset + vma_of_addr->file_nbytes - file_start_offset;
      uint64 nbytes = (remainder > PGSIZE) ? PGSIZE : remainder; 
      //printf("**********************\n");
      //print_memory_areas(p);
      //printf(" file_offs = %p | addr = %p | vma_begin = %p | file_start = %p | remainder = %p | nbytes = %p\n", 
      // vma_of_addr->file_offset, addr, vma_of_addr->va_begin, file_start_offset, remainder, nbytes);
      
      release(&p->vma_lock);
      int res = load_from_file(vma_of_addr->file, file_start_offset, (uint64) pa, nbytes);
      acquire(&p->vma_lock);
      if (res != 0){
        kfree((char*) pa);
        return ENOFILE;
      }
    }
    return 0;
  }
  
  if (vma_of_addr == 0){printf("NO VMA\n");return ENOVMA;}

  if (vma_of_addr->vma_flags != 0 && 
     ((scause == CAUSE_R && (PTE_R & vma_of_addr->vma_flags) == 0) || 
      (scause == CAUSE_W && (PTE_W & vma_of_addr->vma_flags) == 0) ||
      (scause == CAUSE_X && (PTE_X & vma_of_addr->vma_flags) == 0)))
        return EBADPERM;

  if ((PTE_U & *pte) == 0){
    return EBADPERM;
  }

  //printf("******************************\n");
  //printf("in do_allocate: addr = %p, already allocated\n", addr);
  //print_memory_areas(p);
  return 0;
}

int do_allocate_range(pagetable_t pagetable, struct proc* p, uint64 addr, uint64 len, uint64 scause){
  uint64 sup = PGROUNDUP(addr + len);
  addr = PGROUNDDOWN(addr);
  for (; addr < sup; addr += PGSIZE){
    acquire(&p->vma_lock);
    if (do_allocate(pagetable, p, addr, scause) != 0){
      release(&p->vma_lock);
      return -1;
    } 
    release(&p->vma_lock);
  }
  return 0;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;

  int f = do_allocate_range(pagetable, myproc(), dstva, len, CAUSE_W);
  if(f < 0) return -1;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
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

  int f = do_allocate_range(pagetable, myproc(), srcva, len, CAUSE_R);
  if(f < 0) return -1;

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
  int num_allocated = 0;
  acquire(&myproc()->vma_lock);
  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    int f = do_allocate(pagetable, myproc(), va0, CAUSE_R);
    if(f < 0) {
      release(&myproc()->vma_lock);
     return -1;
    }
    num_allocated += f;
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0){
      release(&myproc()->vma_lock);
      return -1;
    }
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
  release(&myproc()->vma_lock);
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

void vmprint(pagetable_t pt, uint64 pid, char* cmd){
  printf("page table for pid=%d, cmd=%s, @%p\n", pid, cmd, pt);
  for(int i = 0; i < PGSIZE/sizeof(uint64); i++){
    pte_t pgd = pt[i];
    if(pgd != 0){
      printf("..0x%x:\n", i);
      for(int j = 0; j < 512; j++){
        pte_t pmd = ((uint64*)(PTE2PA(pgd)))[j];
        if(pmd != 0){
          printf(".. ..0x%x: \n", j);
          for(int k = 0; k < 512; k++){
            pte_t pte = ((uint64*)(PTE2PA(pmd)))[k];
            if(pte != 0){
              printf(".. .. ..0x%x:\t V=%x R=%x W=%x X=%x U=%x VAs=[%p; %p]\n",
                     k,
                     (pte & PTE_V) != 0,
                     (pte & PTE_R) != 0,
                     (pte & PTE_W) != 0,
                     (pte & PTE_X) != 0,
                     (pte & PTE_U) != 0,
                     ((((i << 9) + j) << 9) + k) << 12,
                     (((((i << 9) + j) << 9) + k + 1) << 12) - 1
                );
            }
          }
        }
      }
    }
  }
}
