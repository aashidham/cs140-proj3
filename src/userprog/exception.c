#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "userprog/syscall.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include "threads/vaddr.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/string.h"
#include "lib/round.h"
#include "filesys/off_t.h"


/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void) 
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void) 
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f) 
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */
     
  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      thread_exit (); 

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel"); 

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      thread_exit ();
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f) 
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */

  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));
 //if(fault_addr==0)
	//exit(-1);
	
  //if(!check_pointer(fault_addr))
  	//exit(-1);
  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();
  struct thread *t = thread_current ();
  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;
  
  /* To implement virtual memory, delete the rest of the function
     body, and replace it with code that brings in the page to
     which fault_addr refers. */
  //printf ("Page fault at %p: %s | %s | %s (esp:%p) (eip:%p) \n",(void*)ROUND_DOWN((uintptr_t)fault_addr,PGSIZE),not_present ? "not present page" : "writing r/o page",write ? "writing access" : "reading access",user ? "user access" : "kernel access",f->esp,f->eip);
  if(user && !syscall_check_pointer(fault_addr,f->esp))
  {
    //printf("invalid address %p, rounded is %p, compared to esp is %p\n",fault_addr,(void*)ROUND_DOWN((uintptr_t)fault_addr,PGSIZE),f->esp);
   	exit(-1);
  }

   uint8_t *kpage = get_page (PAL_USER);
   if (kpage == NULL)
	 exit(-1);
  uint8_t *upage = (uint8_t*)ROUND_DOWN((uintptr_t)fault_addr, PGSIZE);
  if(read_page_from_swap(upage,kpage,t))
  {
	struct list_elem *e;
	for(e = list_begin(&swap_table); e != list_end(&swap_table); e = list_next(e))
	{
		struct swap_table_entry *curr = list_entry(e,struct swap_table_entry,elem);
		//if (curr->taken) printf("curr in swap table is taken with upage %p in thread %s\n",curr->upage,curr->t->name);
		if(curr->upage == upage && curr->t == t && curr->taken)
		{
			if (!install_page_handler (upage, kpage, curr->writable)) 
			{
			  palloc_free_page (kpage);
			  exit(-1);
			}
			//printf("Swap recovery successful for %p! Here is a character: %c\n",upage,*(char*)kpage);
			
			//free the space up
			curr->taken = false;
			return;
		}
	}
  }
  
  //bool found = false;
  struct list_elem *e;
  for (e = list_begin (&t->supp_page_table); e != list_end (&t->supp_page_table);
           e = list_next (e))
        {
          struct supp_page_table_entry *curr = list_entry(e,struct supp_page_table_entry,elem);
          upage = curr->upage;
          if(upage == (uint8_t *) ROUND_DOWN((uintptr_t)fault_addr, PGSIZE))
          {
          	size_t page_read_bytes = curr->page_read_bytes;
          	size_t page_zero_bytes = curr->page_zero_bytes;
          	bool writable = curr->writable;
          	//printf("page fault found at %p! (%s) (offset: %d) (eip:%d,%p)\n",upage,writable? "writable":"not writable",(int)curr->ofs,f->cs,f->eip);
          	//found = true;
			/* Get a page of memory. */

			struct file* file;
			if(!curr->mmaped_file)
				file = t->my_binary;
			else
				file = curr->mmaped_file;
			if (file == NULL)
				exit(-1);
			file_seek (file,(int)curr->ofs);
			/* Load this page. */
			
			if (file_read (file, kpage, page_read_bytes) != (off_t) page_read_bytes)
			{
			  palloc_free_page (kpage);
			  exit(-1);
			}
			memset (kpage + page_read_bytes, 0, page_zero_bytes);

			/* Add the page to the process's address space. */
			if (!install_page_handler (upage, kpage, writable)) 
			{
			  palloc_free_page (kpage);
			  exit(-1);
			}
			return;
          }
        }
        
        palloc_free_page(kpage);
    
    //if fault is not in supp table, it has to be part of stack
   		//printf("page fault %p (rounded %p) found as stack extension! esp:%p\n",fault_addr,(void*)ROUND_DOWN((uintptr_t)fault_addr,PGSIZE),f->esp);
   		kpage = get_page (PAL_USER|PAL_ZERO);
   		if (kpage == NULL)
			exit(-1);
		upage = (void*)ROUND_DOWN((uintptr_t)fault_addr,PGSIZE);
		if (!install_page_handler (upage, kpage, write)) 
		{
			palloc_free_page (kpage);
			exit(-1);
		}
		return;
}


