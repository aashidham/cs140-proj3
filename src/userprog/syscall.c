#include "userprog/syscall.h"
#include "userprog/process.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"

#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#define min(a,b)	(a>b)?b:a

unsigned BUFFER_SIZE	=	256;

/*indicates the number of arguments required by each system call. Comments copied from syscall-nr.h*/
int num_args[]	=
{
	0,	/* Halt the operating system. */
    1,	/* Terminate this process. */
	1,	/* Start another process. */
	1,	/* Wait for a child process to die. */
	2,	/* Create a file. */
	1,	/* Delete a file. */
	1,	/* Open a file. */
	1,	/* Obtain a file's size. */
	3,	/* Read from a file. */
	3,	/* Write to a file. */
	2,	/* Change position in a file. */
	1,	/* Report current position in a file. */
	1,	/* Close a file. */
};

/* utility functions */
struct file * get_file_pointer(int fd);
struct file_elem * get_file_element(int fd);
void close_files(struct thread *t);
bool check_pointer(void *ptr);
bool check_filename(const char *file);
int stdout_write (const char *buffer, unsigned size);


/* system call handlers */
static void syscall_handler (struct intr_frame *);
int write (int fd, const void *buffer, unsigned size);
void exit (int status);
bool create (const char *file, unsigned initial_size);
bool remove (const char *file);
int open (const char *file);
int read (int fd, void *buffer, unsigned size);
void seek (int fd, unsigned position);
unsigned tell (int fd);
int filesize (int fd);
int exec (const char * cmd_line);
void close (int fd);
int wait (int pid);
void halt(void);


/* this function checks if the pointer passed to it is valid or not. */
bool check_pointer(void *ptr)
{
	struct thread *t = thread_current();
	if(!ptr)									return false;
	if(!is_user_vaddr(ptr))						return false;
	if(!pagedir_get_page(t->pagedir, ptr))		return false;
	
	return true;
}
/* closes the files opened by thread t	*/
void close_files(struct thread *t)
{
	struct list_elem *e;
	
	/*
	iterates through the list of the files opened by the thread and closes each one of them along with freeing the memory associated.
	*/
	for (e = list_begin(&t->file_list); e != list_end(&t->file_list);)
	{
		struct list_elem *temp=list_next(e);
		struct file_elem *ele=list_entry(e, struct file_elem, elem);
		file_close(ele->file_pointer);
		list_remove(&ele->elem);
		free(ele);
		e=temp;
	}
}

/* writes the buffer to STDOUT using the putbuf function. */
int stdout_write (const char *buffer, unsigned size)
{
	unsigned total	=	0;
	while (size > 0)
	{
		unsigned  size_to_be_written	=	min(BUFFER_SIZE,size);
		
		/* pushes BUFFER_SIZE characters to the standard output stream. */
		putbuf (buffer, size_to_be_written);
		buffer = (const char*)buffer + BUFFER_SIZE;
		size -= size_to_be_written;
		total+=size_to_be_written;
	}
	return total;
}

/* checks if the length of the filename is within legal limits. */
bool check_filename(const char *file)
{
	if(!check_pointer((void *)file))
		exit(-1);
	return (strlen(file)>0)&&(strlen(file)<15);
}

/* returns of the file_pointer represented by fd. */
struct file * get_file_pointer(int fd)
{
	struct list_elem *e;
	
	/* 
	iterates through the list of file descriptors of the current thread and finds the file pointer associated with FD
	*/
	for (e = list_begin(&thread_current()->file_list); e != list_end(&thread_current()->file_list); e = list_next (e))
	{
		if(list_entry(e, struct file_elem, elem)->fd==fd)
			return list_entry(e,struct file_elem,elem)->file_pointer;
	}
	
	/*
	if the fd is not found in the list, return NULL 
	*/
	return NULL;
}
struct file_elem * get_file_element(int fd)
{
	struct list_elem *e;
	
	/* 
	iterates through the list of file descriptors of the current thread and return file_elem associated with the file.
	*/
	for (e = list_begin(&thread_current()->file_list); e != list_end(&thread_current()->file_list); e = list_next (e))
	{
		if(list_entry(e, struct file_elem, elem)->fd==fd)
			return (struct file_elem *)list_entry(e,struct file_elem,elem);
	}
	return NULL;
}


/* creates a file with the filename provided. */
bool create (const char *file, unsigned initial_size)
{
	if(!check_filename(file))
		return false;
	return filesys_create(file, initial_size);
}

/* removes the file with the filename provided. */
bool remove (const char *file)
{
	if(!check_filename(file))
		return false;
	struct file *f=filesys_open(file);
	if(!f)
		return false;
	file_close(f);
	return filesys_remove(file);
}

/* 
This function opens the file with the given filename by allocating a fd for the opened file. This function returns the fd that got allocated to represent the file.
*/
int open (const char *file)
{
	if(!check_filename(file))
		return -1;
	struct file *f=filesys_open (file);
	if(!f)
		return -1;
	
	struct file_elem *temp=(struct file_elem *)malloc(sizeof (struct file_elem));
	int id=thread_current()->FD_CURRENT;
	thread_current()->FD_CURRENT++;
	temp->fd=id;
	temp->file_pointer=f;
	
	/* push the file element object into the current thread's file_list. */
	list_push_back(&thread_current()->file_list,&temp->elem);
	return id;
}

/* exit system call. calls user_process_exit(status) defined in process.c */
void exit (int status)
{
	user_process_exit(status);
	thread_exit();
}

/* write system call */
int write (int fd, const void *buffer, unsigned size)
{
	if(!check_pointer((void *)buffer))
		exit(-1);
		
	/* In case of writing to STDOUT, call stdout_write() */
	if(fd==1)
		return stdout_write((const char *)buffer,size);
	else
	if(fd==0)
		exit(-1);

	/* write to the file indicated by the file descriptor. */
	struct file *f=get_file_pointer(fd);
	if(!f)
		return -1;
	return file_write (f,buffer,size);
	//printf("write here\n");
}

/* read system call. performs read from the input stream. */
int read (int fd, void *buffer, unsigned size)
{
	if(!check_pointer(buffer)||fd==STDOUT_FILENO)
		exit(-1);
	
	/*In case of reading from the stanford input, input_getc() gets used to read characters from the STDIN. */
	if(fd==STDIN_FILENO)
	{
		unsigned i=0;
		char *buf=(char *)buffer;
		for(i=0;i<size;i++)
			buf[i]=(char)input_getc();
		return size;
	}
	
	/* Read from the file indicated by its FD. */
	struct file *f=get_file_pointer(fd);
	if(f==NULL)
		return -1;
	int read =	file_read(f,buffer,size);
	buffer += read;
	return read;
}

/* performs the exec system call. */
int exec (const char *cmd_line)
{
	if(!check_pointer((void *)cmd_line))
		exit(-1);
	int t=process_execute(cmd_line);
	if(t!=TID_ERROR)
		return t;
	return -1;
}

/* returns the filesize of the file indicated by its fd. */
int filesize (int fd)
{
	struct file *f=get_file_pointer(fd);
	if(!f)
		return -1;
	return file_length(f);
}

/* seeks to a location within an opened file. The file gets indicated by its file descriptor. */
void seek (int fd, unsigned position)
{
	struct file *f=get_file_pointer(fd);
	if(!f)
		return;
	file_seek(f,position);
}

/* tell system call. */
unsigned tell (int fd)
{
	struct file *f=get_file_pointer(fd);
	if(!f)
		exit(-1);
	return file_tell(f);
}

/* shuts down all the processes. */
void halt()
{
	shutdown_power_off();
}

/* waits for the thread pointed by pid to complete. */
int wait (int pid)
{
	return process_wait(pid);
}

/* closes the opened file. */
void close (int fd)
{
	struct file_elem *ele=get_file_element(fd);
	if(!ele)
		return;
	file_close(ele->file_pointer);
	list_remove(&ele->elem);
	free(ele);
}


void
syscall_init (void) 
{
	intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}
static void
syscall_handler (struct intr_frame *f UNUSED) 
{
	int i=0;
	if(!check_pointer(f->esp))
		exit(-1);

	/* finds the system call number by dereferencing the stack pointer. */
	int syscall_num = *(int *)f->esp;
	
	/* finds the number of arguments required by the system call. */
	int num_arguments=num_args[syscall_num];
	unsigned int arguments[num_arguments];
	for(i=0;i<num_arguments;i++)
	{
		if(!check_pointer(f->esp + (4*(i+1))))
			exit(-1);
		arguments[i]=*((unsigned int *)(f->esp + (4*(i+1))));
	}
	
	//printf("syscall number %d arguments %d\n",syscall_num,num_arguments);
	
	/* calls the appropriate system call. */
	switch(syscall_num)
	{
		case 0:		halt();																	return;
		case 1:		exit(arguments[0]);														return;
		case 2:		f->eax=exec((const char *)arguments[0]);								return;
		case 3:		f->eax=wait((int)arguments[0]);											return;

		case 4:		f->eax=create((const char *)arguments[0],arguments[1]);					return;
		case 5:		f->eax=remove((const char *)arguments[0]);								return;
		case 6:		f->eax=open((const char *)arguments[0]);								return;
		case 7:		f->eax=filesize((int)arguments[0]);										return;

		case 8:		f->eax=read((int)arguments[0], (void *)arguments[1], arguments[2]);		return;
		case 9:		f->eax=write((int)arguments[0],(const void *)arguments[1],arguments[2]);return;
		case 10:	seek((int)arguments[0],arguments[1]);									return;
		case 11:	f->eax=tell((int)arguments[0]);											return;

		case 12:	close((int)arguments[0]);												return;
		default:	break;
	};
	exit(-1);
}
