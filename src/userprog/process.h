#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "filesys/off_t.h"
#include "threads/thread.h"

tid_t process_execute (const char *file_name);
int process_wait (tid_t);
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
	const char* file_name;
	off_t ofs;
	bool writable;
};


#endif /* userprog/process.h */
