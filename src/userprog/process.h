#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "filesys/off_t.h"
#include "threads/thread.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
void user_process_exit(int exit_code);
void process_exit (void);
void process_activate (void);
bool install_page_handler (void *upage, void *kpage, bool writable);
struct list frame_table;			/* Lists frames that contain a user page*/

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
