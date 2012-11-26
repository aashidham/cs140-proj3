#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "filesys/off_t.h"
#include "threads/thread.h"
#include "devices/block.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void user_process_exit(int exit_code);
void evict_algorithm(void);
void process_exit (void);
void process_activate (void);
bool install_page_handler (void *upage, void *kpage, bool writable);
bool read_page_from_swap(void* upage, void* kpage, struct thread *t);
bool write_page_to_swap(void* upage, struct thread *t);

struct list frame_table;			/* Lists frames that contain a user page*/
struct list swap_table;
struct block *swap_block;

struct swap_table_entry
{
	struct list_elem elem;
	void *upage;
	struct thread *t;
	bool writable;
	block_sector_t slot;
	bool taken;
};


struct supp_page_table_entry
{
	struct list_elem elem;
	uint8_t *upage;
	size_t page_read_bytes;
	size_t page_zero_bytes;
	off_t ofs;
	bool writable;
	struct file *mmaped_file; //these only filled if comes from mmap syscall
	int mmaped_id;
};


#endif /* userprog/process.h */
