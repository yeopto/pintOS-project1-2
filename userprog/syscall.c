#include "userprog/syscall.h"
#include "threads/palloc.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "filesys/filesys.h"
#include "filesys/file.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/synch.h"
#include "userprog/process.h"

int add_file_to_fdt(struct file *file);
void remove_file_from_fdt(int fd);
static struct file *find_file_by_fd(int fd);
void check_address(uaddr);
void halt(void);
void exit(int status);
bool create(const char *file, unsigned initial_size);
bool remove(const char *file);
int open(const char *file);
int filesize(int fd);
int read(int fd, void *buffer, unsigned size);
int write(int fd, const void *buffer, unsigned size);
void seek(int fd, unsigned position);
unsigned tell(int fd);
void close(int fd);
int exec (char *file_name);
int wait(int tid);
tid_t fork (const char *thread_name, struct intr_frame *f);

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

	lock_init(&filesys_lock); // ????????? ?????????
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) { // ???????????? ?????? ??????
	// TODO: Your implementation goes here.
	// printf ("system call!\n");
	char *fn_copy;

	/*
	x86-64 ????????? ????????? ???????????? ?????? rax ??????????????? ???????????? ???
	?????? ???????????? ????????? ?????? intr_frame ???????????? rax ?????? ???????????? ??????
	 */
	switch (f->R.rax) {		// rax is the system call number
		case SYS_HALT:
			halt();			// pintos??? ??????????????? ????????? ???
			break;
		case SYS_EXIT:
			exit(f->R.rdi);	// ?????? ??????????????? ??????????????? ????????? ???
			break;
		case SYS_FORK:
			f->R.rax = fork(f->R.rdi, f); // ?????? ????????? ???????????? ????????? ?????????, ???????????? ???????????????
			break;
		case SYS_EXEC:
			if (exec(f->R.rdi) == -1) {
				exit(-1);
			}
			break;
		case SYS_WAIT:
			f->R.rax = wait(f->R.rdi);
			break;
		case SYS_CREATE:
			f->R.rax = create(f->R.rdi, f->R.rsi);
			break;
		case SYS_REMOVE:
			f->R.rax = remove(f->R.rdi);
			break;
		case SYS_OPEN:
			f->R.rax = open(f->R.rdi);
			break;
		case SYS_FILESIZE:
			f->R.rax = filesize(f->R.rdi);
			break;
		case SYS_READ:
			f->R.rax = read(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_WRITE:
			f->R.rax = write(f->R.rdi, f->R.rsi, f->R.rdx);
			break;
		case SYS_SEEK:
			seek(f->R.rdi, f->R.rsi);
			break;
		case SYS_TELL:
			f->R.rax = tell(f->R.rdi);
			break;
		case SYS_CLOSE:
			close(f->R.rdi);
			break;
		default:
			exit(-1);
			break;
	// thread_exit ();
	}
}

// ???????????? ?????? ??????(0x8048000~0xc0000000)?????? ???????????? ??????????????? ???????????? ??????
void check_address(const uint64_t *uaddr)	
{
	struct thread *cur = thread_current();
	if (uaddr == NULL || !(is_user_vaddr(uaddr)) || pml4_get_page(cur->pml4, uaddr) == NULL)
	{
		exit(-1);
	}
}

void halt(void) { // pintos ?????? ????????? ???
	power_off();
}

void exit(int status) { // ??????????????? ??????????????? ????????? ???
	struct thread *cur = thread_current();
	cur->exit_status = status; 

	printf("%s: exit(%d)\n", thread_name(), status); // plj2 - process termination messages
	thread_exit(); // thread ?????? -> process_exit() ??????
}

tid_t fork(const char *thread_name, struct intr_frame *f) {
	return process_fork(thread_name, f);
}

int exec(char *file_name) { // ?????? ??????????????? ????????????????????? ????????? ????????? ???????????? ????????? ????????? ?????? ????????? ??????
	check_address(file_name);

	int file_size = strlen(file_name) + 1; // NULL ?????? + 1
	char *fn_copy = palloc_get_page(PAL_ZERO);

	if (fn_copy == NULL) {
		exit(-1);
	}
	strlcpy(fn_copy, file_name, file_size);

	if (process_exec(fn_copy) == -1) {
		return -1;
	}

	NOT_REACHED();
	return 0;
}

int wait(int tid) {
	return process_wait(tid);
}

/*
?????? ????????? ?????? ???????????? ?????? ????????? ?????? ????????? ???????????? ??????. 
filesys_create ????????? ?????? ????????? ?????? ???????????? ??????????????? ?????? ????????? ???????????? ??????
*/
// ?????? ???????????? ????????? ???
// ????????? ?????? true, ????????? ?????? false ??????
bool create(const char *file, unsigned initial_size) { // file: ????????? ????????? ?????? ??? ?????? ??????, initial_size: ????????? ?????? ??????
	check_address(file);
	return filesys_create(file, initial_size);
}

/* ????????? ???????????? ????????? ??????, file ????????? ????????? ????????? ?????? ??? ?????? ????????????. ????????? ??? true, ?????? ??? false ?????? */
bool remove(const char *file) {
	check_address(file);
	return filesys_remove(file);
}

int add_file_to_fdt(struct file *file) {
	struct thread *cur = thread_current();
	struct file **fdt = cur->fd_table;
	// fd??? ????????? ?????? ????????? ?????? ??????, fdtable??? ????????? ????????? ???????????????
	while(cur->fd_idx < FDCOUNT_LIMIT && fdt[cur->fd_idx]) {
		cur->fd_idx++; // open-twice -> ??????????????? ?????? ???????????? ?????? 2??? ?????????????????? ????????? ???????????? ++??? ????????? ?????????
	}

	if (cur->fd_idx >= FDCOUNT_LIMIT)
		return -1;
	
	fdt[cur->fd_idx] = file;
	return cur->fd_idx;
}

/* ????????? ??? ??? ???????????? ????????? ???. ?????? ??? fd??? ???????????? ??????, ?????? ??? -1 ?????? */
int open(const char *file) {
	check_address(file); // ????????? ???????????? ???????????????
	struct file *open_file = filesys_open(file); // filesys_open?????? file??? ?????? ??????????????? ????????? ????????? ????????????

	// open_null ????????? ?????? ??????
	if (file == NULL) {
		return -1;
	}

	if (open_file == NULL) {
		return -1;
	}

	int fd = add_file_to_fdt(open_file);

	if (fd == -1) {
		file_close(open_file);
	}

	return fd;
}

// fd??? ?????? ?????? ??????
static struct file *find_file_by_fd(int fd) {
	struct thread *cur = thread_current();

	if (fd < 0 || fd >= FDCOUNT_LIMIT) {
		return NULL;
	}
	return cur->fd_table[fd];
}

/* ?????? ????????? ???????????? ????????? ??? */
// fd????????? ?????? ?????? ?????? ??????
int filesize(int fd) {
	struct file *open_file = find_file_by_fd(fd);
	if (open_file == NULL) {
		return -1;
	}
	return file_length(open_file);
}

// ??????: ?????? ??? ?????? pid ??????, ?????? ??? -1
// ??????: ?????? ??? 0 ??????

// read()??? ?????? ????????? ???????????? ?????? ????????? ???
int read(int fd, void *buffer, unsigned size) { // buffer??? ?????? ???????????? ????????? ????????? ?????????, size??? ?????? ???????????? ??????
	check_address(buffer);
	off_t read_byte;
	uint8_t *read_buffer = buffer;
	if (fd == 0) { // fd ?????? 0??? ?????? ?????????????????? ????????? input_getc() ????????? ???????????? ???????????? ???????????? ?????? ????????? ?????????.
		char key;
		for (read_byte = 0; read_byte < size; read_byte++) {
			key = input_getc();
			*read_buffer++ = key;
			if (key == '\0') {
				break;
			}
		}
	}
	else if (fd == 1) { // tests/userprog/read-stdout
		return -1;
	}
	else {
		struct file *read_file = find_file_by_fd(fd);
		if (read_file == NULL) {
			return -1;
		} // race-condition??? ????????? ?????? ?????? ?????? lock
		lock_acquire(&filesys_lock);
		read_byte = file_read(read_file, buffer, size);
		lock_release(&filesys_lock);
	}
	return read_byte;
}

// write() ????????? ?????? ????????? ???????????? ???????????? ????????? ???
// buffer????????? ????????? ??????
int write(int fd, const void *buffer, unsigned size)
{
    check_address(buffer);

    int write_result;

    if (fd == 0) // stdin
    {
        return 0;
    }
    else if (fd == 1) // stdout
    {
        putbuf(buffer, size);
        return size;
    }
    else
    {
        struct file *write_file = find_file_by_fd(fd);
        if (write_file == NULL)
        {
            return 0;
        }
        lock_acquire(&filesys_lock);
        off_t write_result = file_write(write_file, buffer, size);
        lock_release(&filesys_lock);
        return write_result;
    }
}

// ?????? ??????(offset)??? ???????????? ??????
/* seek()??? ???????????? ?????? fd??? ????????? ?????? ????????? ????????? ????????? ????????? position ????????? ???????????? ??????*/
/* ????????? ???????????? position ???????????? ?????? ??? ????????? ?????? position??? ?????? ?????? */
void seek(int fd, unsigned position) {
	struct file *seek_file = find_file_by_fd(fd);
	if (seek_file <= 2) {		// ????????? 2??? ??????. 0: ?????? ??????, 1: ?????? ??????
		return;
	}
	seek_file->pos = position;
}

/* ?????? ????????? ????????? ???????????? ????????? ??? */
// ????????? ??????(offset)??? ???????????? ??????
unsigned tell(int fd) {
	struct file *tell_file = find_file_by_fd(fd);
	if (tell_file <= 2) {
		return;
	}
	return file_tell(tell_file);
}

void remove_file_from_fdt(int fd) {
	struct thread *cur = thread_current();

	//error -invalid fd
	if (fd < 0 || fd >= FDCOUNT_LIMIT)
		return;
	cur->fd_table[fd] = NULL;
}

/* close()??? ?????? ????????? ?????? ????????? ?????????. ????????? ?????? fd??? ????????????. */
// ?????? ????????? ?????? ????????? ???. ????????? ?????? fd??????
void close(int fd) {
	struct file *fileobj = find_file_by_fd(fd);
	if (fileobj == NULL) {
		return;
	}
	remove_file_from_fdt(fd);
}
