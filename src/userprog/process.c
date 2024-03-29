#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "devices/partition.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
void evict_algorithm(void);
static struct child *get_child_pointer(tid_t tid);
static void notify_parent(struct thread *my_parent);
static int frame_entry = 0;

struct frame_table_entry
{
	struct list_elem elem;
	void *kpage;
	void *upage;
	struct thread *t;
	bool writable;
};


static struct list args_locations;
static struct list args_list;
struct args_location_elem
{
	struct list_elem elem;
	char *location;
};
struct args_list_elem
{
	struct list_elem elem;
	char *token;
};
/*this function notifies the parent of change in status in the child*/
static void notify_parent(struct thread *my_parent)
{
	lock_acquire(&my_parent->status_change_lock);
	cond_signal(&my_parent->status_change,&my_parent->status_change_lock);
	lock_release(&my_parent->status_change_lock);
}


/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	Otherwise there's a race between the caller and load(). */
	fn_copy = get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

  
	/* Create a new thread to execute FILE_NAME. */
	tid = thread_create (file_name, PRI_DEFAULT, start_process, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy); 
	//printf("A\n");
	/* waits for the thread to change to either PROCESS_STARTED or PROCESS_FAILED state */
	lock_acquire(&thread_current()->status_change_lock);
	//printf("B\n");
	struct child *p=get_child_pointer(tid);
	while(p->state==PROCESS_INITIALIZING)
		cond_wait(&thread_current()->status_change,&thread_current()->status_change_lock);
	//printf("C\n");
	lock_release(&thread_current()->status_change_lock);
	//printf("D\n");
	
	/* checks if the LOAD has not failed */
	if(p->state==PROCESS_FAILED)
	{
		//printf("load failed\n");
		return TID_ERROR;
	}
	return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
	char *file_name = file_name_;
	//printf("In start_process with %s\n",file_name);
	int current=0;
	struct intr_frame if_;
	bool success;
  
	struct list_elem *e;

	/*The arguments in the command line and tokenised and pushed to the args_list stack. */
	list_init (&args_list);
	char *token, *save_ptr;
	for (token = strtok_r (file_name, " ", &save_ptr); token != NULL; token = strtok_r (NULL, " ", &save_ptr))
	{
		if(current==0)
		{
			strlcpy(&(thread_current()->name), (const char *)token, sizeof (thread_current()->name));
			current++;
		}
		struct args_list_elem *temp=(struct args_list_elem *)(malloc(sizeof (struct args_list_elem)));
		temp->token=token;
		list_push_front(&args_list,&temp->elem);
	}
  
  /* Initialize interrupt frame and load executable. */
	memset (&if_, 0, sizeof if_);
	if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
	if_.cs = SEL_UCSEG;
	if_.eflags = FLAG_IF | FLAG_MBS;
	success = load ((thread_current()->name), &if_.eip, &if_.esp);
//printf("H\n");
	struct thread *my_parent=thread_current()->myself->parent;
	
	/* If load failed, quit. */
	if (!success)
	{
		palloc_free_page (file_name);
		close_files(thread_current());
		
		/*
		If the load has failed, the process changes its state to PROCESS_FAILED and notifies the parent of the change in its status. 
		*/
		thread_current()->myself->state		=	PROCESS_FAILED;
		if(my_parent)
			notify_parent(my_parent);
		thread_exit ();
	}
	/*
	If the load is successful, the process changes its state to PROCESS_STARTED and notifies the parent of the change in its status. 
	*/
	thread_current()->myself->state			=	PROCESS_STARTED;
	if(my_parent)
		notify_parent(my_parent);
  
	/* Push the arguments into the memory */
	list_init (&args_locations);
	for (e = list_begin (&args_list); e != list_end (&args_list); e = list_next (e))
	{
		char *temp = list_entry(e, struct args_list_elem, elem)->token;
		if_.esp -= (strlen(temp)+1);
		
		//printf("inside loop addr %x %d\n",(unsigned int)if_.esp,strlen(temp));
		memcpy(if_.esp,temp,strlen(temp)+1);
		
		struct args_location_elem *t=(struct args_location_elem *)malloc(sizeof(struct args_location_elem));
		t->location	= if_.esp;
		list_push_back(&args_locations,&t->elem);
	}
	
	/*
		Align the stack pointer to start at the start of the word.
	*/
	unsigned int temp = (unsigned int)if_.esp;
	if_.esp -= (temp%4);
	if_.esp -=sizeof(int);
	memset(if_.esp, 0, temp - (unsigned int)if_.esp);
	
	/* Push the locations of the arguments into the memory */
	for (e = list_begin (&args_locations); e != list_end (&args_locations); e = list_next (e))
	{
		char *temp = list_entry(e, struct args_location_elem, elem)->location;
		if_.esp -= sizeof (char *);
		memcpy(if_.esp, &temp, sizeof (char *));
	}

	/*
	Push the base address of the arguments into the memory.
	*/
	void *arg=if_.esp;
	if_.esp -= sizeof(char **);
	memcpy(if_.esp,&arg,sizeof(void **));
	
	/*
	Push the number of arguments into the memory.
	*/
	int argc = list_size(&args_locations);
	if_.esp -= sizeof (int);
	memcpy(if_.esp, &argc,sizeof (int));
	
	if_.esp -= sizeof (void (*) ());
	
	/*
	Mark the file as non-writable.
	*/
	thread_current()->my_binary=filesys_open(file_name);
	file_deny_write(thread_current()->my_binary);
	palloc_free_page (file_name);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
    //printf("I\n");
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* This function returns the pointer to the state of the child identified by its tid. */
static struct child *get_child_pointer(tid_t tid)
{
	struct list_elem *e;
	for (e = list_begin (&thread_current()->child_list); e != list_end (&thread_current()->child_list); e = list_next (e))
	{
		struct child *temp = (struct child *)list_entry(e, struct child, elem);
		if(temp->tid==tid)
			return temp;
	}
	return NULL;
}
/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
	lock_acquire(&thread_current()->status_change_lock);
	struct child *p	=	get_child_pointer(child_tid);
	if(!p)
	{
		//printf("Mistake here\n");
		lock_release(&thread_current()->status_change_lock);
		return -1;
	}
	
	/* 
		Wait for child to change its status from PROCESS_STARTED to PROCESS_EXITED
	*/
	while(p->state==PROCESS_STARTED)
		cond_wait(&thread_current()->status_change,&thread_current()->status_change_lock);
	
	/*
		Extract the child's exit code and return it. 
	*/
	int ret	=	p->exit_code;
	list_remove(&p->elem);
	free(p);
	lock_release(&thread_current()->status_change_lock);
	return ret;	
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Exit the user thread and notify the parent of the new status. */
void user_process_exit(int exit_code)
{
	printf("%s: exit(%d)\n", thread_current()->name, exit_code);
	//thread_current()->myself->exit_flag	=	true;
	thread_current()->myself->exit_code		=	exit_code;
	thread_current()->myself->state			=	PROCESS_EXITED;
	struct thread *my_parent=thread_current()->myself->parent;
	if(my_parent)
		notify_parent(my_parent);
	else
		free(thread_current()->myself);
		
	/*
		Free the memory associated with each of the thread's child if the child has completed execution.
	*/
	struct list_elem *e;
	for (e = list_begin(&thread_current()->child_list); e != list_end(&thread_current ()->child_list);)
	{
		struct child *temp=list_entry(e,struct child,elem);
		e=list_next(e);
		if(temp->state!=PROCESS_STARTED)
			free(temp);
		else
			temp->parent=NULL;
	}
	
	/*Close the files that were opened by the thread */
	close_files(thread_current());
	
	/* Allow future process to write to the file and give back the write privileges. */
	file_close(thread_current()->my_binary);
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. 
*/
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

void init_swap_table(void)
{
  if(!swap_block)  swap_block = block_get_role (BLOCK_SWAP);
  int total_sector_pages = (block_size(swap_block) * BLOCK_SECTOR_SIZE ) / PGSIZE;
  int i;
  for(i = 0; i < total_sector_pages; i++)
  {
  	struct swap_table_entry* curr = malloc(sizeof(struct swap_table_entry));
  	curr->slot = i * (PGSIZE/BLOCK_SECTOR_SIZE);
  	curr->taken = 0;
  	list_push_back(&swap_table,&curr->elem);
  }
}

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp) 
{
   //printf("E\n");
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory, supplementary page table and frame table. */
  t->pagedir = pagedir_create ();
  list_init(&t->supp_page_table);
  if (t->pagedir == NULL) 
    goto done;
  if(list_empty(&swap_table)) init_swap_table();
  process_activate ();
  
  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      //printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      //printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
                //printf("Section offset is %d and is %s\n",file_page,writable? "writable":"not writable");
              if (!load_segment (file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }
//printf("F\n");
  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;
//printf("G\n");
  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  file_close (file);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;
      
      struct supp_page_table_entry *curr = malloc(sizeof(struct supp_page_table_entry));
      curr->upage = upage;
      curr->page_read_bytes = page_read_bytes;
      curr->page_zero_bytes = page_zero_bytes;
      curr->ofs = ofs;
      curr->writable = writable;
      curr->mmaped_file = NULL;
      curr->mmaped_id = 0;
      list_push_back(&thread_current()->supp_page_table,&curr->elem);


      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
      ofs += page_read_bytes;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

bool install_page_handler(void *upage, void *kpage, bool writable)
{
	return install_page (upage, kpage, writable);
}


/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();
  //printf("!\n");
  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  bool success = (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
 if(success)
 {
 	struct frame_table_entry* curr = malloc(sizeof(struct frame_table_entry));
 	curr->kpage = kpage;
 	curr->upage = upage;
 	curr->t = thread_current();
 	curr->writable = writable;
 	list_push_back(&frame_table,&curr->elem);
 	//printf("Page table entry for %s thread at %p\n",t->name,upage);
 }
 else
 {
 	//printf("%p already exists inside %s's page table\n",upage,t->name);
 	}
 /*struct list_elem *e;
 for (e = list_begin (&frame_table); e != list_end (&frame_table);
           e = list_next (e))
        {
          struct frame_table_entry *curr = list_entry(e,struct frame_table_entry,elem);
          printf("frame table elem %p\n",curr->kpage);
        }*/

 return success;
}

/* If possible, write the page at upage to the swap device. Must have created swap_table_entry with upage. Returns true on success. */
bool write_page_to_swap(void* upage, struct thread *t)
{
	struct list_elem *e;
	for(e = list_begin(&swap_table); e != list_end(&swap_table); e = list_next(e))
	{
		struct swap_table_entry *curr = list_entry(e,struct swap_table_entry,elem);
		if(curr->upage == upage && curr->t == t)
		{
			int i;
			int num_sectors = PGSIZE/BLOCK_SECTOR_SIZE;
			for(i = 0; i < num_sectors; i++)
			{
				block_sector_t slot = curr->slot + i;
				block_write(swap_block,slot, (char*) upage + i*BLOCK_SECTOR_SIZE);
			}
			return true;
		}
	}
	return false;
}

/*Find the address at upage if present in the swap device, and write it to kpage. The user should also free the entry for other memory. Return true on success.*/
bool read_page_from_swap(void* upage, void* kpage, struct thread *t)
{
	struct list_elem *e;
	for(e = list_begin(&swap_table); e != list_end(&swap_table); e = list_next(e))
	{
		struct swap_table_entry *curr = list_entry(e,struct swap_table_entry,elem);
		if(curr->upage == upage && curr->t == t)
		{
			int i;
			int num_sectors = PGSIZE/BLOCK_SECTOR_SIZE;
			for(i = 0; i < num_sectors; i++)
			{
				block_sector_t slot = curr->slot + i;
				block_read(swap_block,slot, (char*) kpage + i*BLOCK_SECTOR_SIZE);
			}
			return true;
		}
	}
	return false;
}

void evict_algorithm()
{	
	bool begin = false;
	int i=0;
	struct list_elem *e = list_begin (&frame_table);
	while(true)
    {
    	if(i==frame_entry)
    		begin = true;
    	if(!begin)
    		i++;
    	else
    	{
    		if (e == list_end (&frame_table))
    			e = list_begin (&frame_table);
    		
    		struct frame_table_entry *curr = list_entry(e,struct frame_table_entry,elem);
    		frame_entry = (frame_entry+1)%(list_size(&frame_table)-1);
    		if(curr->t == thread_current() && pagedir_is_accessed(curr->t->pagedir,curr->upage))
    		{
    			pagedir_set_accessed(curr->t->pagedir,curr->upage,false);
    			e = list_next(e);
    		}
    		else
    		{
    			if(curr->t == thread_current() && pagedir_is_dirty(curr->t->pagedir,curr->upage)) 
    			{
    					struct list_elem *e;
    					bool space_in_swap = false;
    					
    					//add entry to swap table
						for(e = list_begin(&swap_table); e != list_end(&swap_table); e = list_next(e))
						{
							struct swap_table_entry *curr_swap = list_entry(e,struct swap_table_entry,elem);
							if(!curr_swap->taken)
							{
								space_in_swap = true;
								curr_swap->upage = curr->upage;
								curr_swap->t = curr->t;
								curr_swap->writable = curr->writable;
								curr_swap->taken = true;
								break;
							}
						}
						if(!space_in_swap) PANIC("Swap full");
						
						//write to swap device
						if(!write_page_to_swap(curr->upage,curr->t)) PANIC("Swap write failed");
						//else printf("Swap creation successful for %p upage! Here is a char: %c\n",curr->upage,*(char*)curr->upage);
    			}
    			//evict page and return
			   void *kpage =  pagedir_get_page(curr->t->pagedir,curr->upage);
			   //printf("eviction succeeded for %p!\n",curr->upage);
			   palloc_free_page(kpage);
			   pagedir_clear_page(curr->t->pagedir,curr->upage);
			   list_remove(e);
			   free(curr);
			   return;
    		}
    	}
    }
}
