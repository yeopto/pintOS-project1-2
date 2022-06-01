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

	lock_init(&filesys_lock); // 초기화 해주기
}

/* The main system call interface */
void
syscall_handler (struct intr_frame *f UNUSED) { // 어셈에서 이거 실행
	// TODO: Your implementation goes here.
	// printf ("system call!\n");
	char *fn_copy;

	/*
	x86-64 규약은 함수가 리턴하는 값을 rax 레지스터에 배치하는 것
	값을 반환하는 시스템 콜은 intr_frame 구조체의 rax 멤버 수정으로 가능
	 */
	switch (f->R.rax) {		// rax is the system call number
		case SYS_HALT:
			halt();			// pintos를 종료시키는 시스템 콜
			break;
		case SYS_EXIT:
			exit(f->R.rdi);	// 현재 프로세스를 종료시키는 시스템 콜
			break;
		case SYS_FORK:
			f->R.rax = fork(f->R.rdi, f); // 자식 스레드 이름으로 인자가 들어감, 인터럽트 프레임이랑
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

// 주소값이 유저 영역(0x8048000~0xc0000000)에서 사용하는 주소값인지 확인하는 함수
void check_address(const uint64_t *uaddr)	
{
	struct thread *cur = thread_current();
	if (uaddr == NULL || !(is_user_vaddr(uaddr)) || pml4_get_page(cur->pml4, uaddr) == NULL)
	{
		exit(-1);
	}
}
void halt(void) { // pintos 종료 시스템 콜
	power_off();
}

void exit(int status) { // 프로세스를 종료시키는 시스템 콜
	struct thread *cur = thread_current();
	cur->exit_status = status; // 종료시 상태를 확인, 정상 종료면 state = 0 

	printf("%s: exit(%d)\n", thread_name(), status); // 종료 메시지 출력
	thread_exit(); // thread 종료
}

tid_t fork(const char *thread_name, struct intr_frame *f) {
	return process_fork(thread_name, f);
}

int exec(char *file_name) { // 현재 프로세스를 커맨드라인에서 지정된 인수를 전달하여 이름이 지정된 실행 파일로 변경
	check_address(file_name);

	int file_size = strlen(file_name) + 1;
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
파일 이름과 파일 사이즈를 인자 값으로 받아 파일을 생성하는 함수. 
filesys_create 함수는 파일 이름과 파일 사이즈를 인자값으로 받아 파일을 생성하는 함수
*/
// 파일 생성하는 시스템 콜
// 성공일 경우 true, 실패일 경우 false 리턴
bool create(const char *file, unsigned initial_size) { // file: 생성할 파일의 이름 및 경로 정보, initial_size: 생성할 파일 크기
	check_address(file);
	return filesys_create(file, initial_size);
}

/* 파일을 삭제하는 시스템 콜로, file 인자는 제거할 파일의 이름 및 경로 정보이다. 성공일 시 true, 실패 시 false 리턴 */
bool remove(const char *file) {
	check_address(file);
	return filesys_remove(file);
}

int add_file_to_fdt(struct file *file) {
	struct thread *cur = thread_current();
	struct file **fdt = cur->fd_table;
	// fd의 위치가 제한 범위를 넘지 않고, fdtable의 인덱스 위치와 일치한다면
	while(cur->fd_idx < FDCOUNT_LIMIT && fdt[cur->fd_idx]) {
		cur->fd_idx++; // open-twice -> 같은파일을 두번 열었는데 둘다 2를 리턴하면안돼 그래서 인덱스를 ++를 해줘서 다르게
	}

	if (cur->fd_idx >= FDCOUNT_LIMIT)
		return -1;
	
	fdt[cur->fd_idx] = file;
	return cur->fd_idx;
}

/* 파일을 열 때 사용하는 시스템 콜. 성공 시 fd를 생성하고 반환, 실패 시 -1 반환 */
int open(const char *file) {
	check_address(file); // 사용자 영역인지 확인해보자
	struct file *open_file = filesys_open(file); // filesys_open으로 file을 진짜 오픈해줘서 오픈된 파일을 리턴해줌

	// open_null 테스트 패스 위해
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

// fd로 파일 찾는 함수
static struct file *find_file_by_fd(int fd) {
	struct thread *cur = thread_current();

	if (fd < 0 || fd >= FDCOUNT_LIMIT) {
		return NULL;
	}
	return cur->fd_table[fd];
}

/* 파일 크기를 알려주는 시스템 콜 */
// fd인자를 받아 파일 크기 리턴
int filesize(int fd) {
	struct file *open_file = find_file_by_fd(fd);
	if (open_file == NULL) {
		return -1;
	}
	return file_length(open_file);
}

// 부모: 성공 시 자식 pid 반환, 실패 시 -1
// 자식: 성공 시 0 반환

// read()는 열린 파일의 데이터를 읽는 시스템 콜
int read(int fd, void *buffer, unsigned size) { // buffer는 읽은 데이터를 저장할 버퍼의 주소값, size는 읽을 데이터의 크기
	check_address(buffer);
	off_t read_byte;
	uint8_t *read_buffer = buffer;
	if (fd == 0) { // fd 값이 0일 때는 표준입력이기 때문에 input_getc() 함수를 이용하여 키보드의 데이터를 읽어 버퍼에 저장함.
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
		} // race-condition을 피하기 위해 읽을 동안 lock
		lock_acquire(&filesys_lock);
		read_byte = file_read(read_file, buffer, size);
		lock_release(&filesys_lock);
	}
	return read_byte;
}

// write() 함수는 열린 파일의 데이터를 기록하는 시스템 콜
// buffer로부터 사이즈 쓰기
// int write(int fd, const void *buffer, unsigned size) {
// 	check_address(buffer);
	
// 	lock_acquire(&filesys_lock); // 쓸 때 동안은 락

// 	int write_result;
	
// 	if (fd == 0) { // tests/userprog/write-stdin
// 		return 0;
// 	}
// 	else if(fd == 1) { // fd 값 1은 표준 출력
// 		putbuf(buffer, size);		// 문자열을 화면에 출력하는 함수
// 		write_result = size;
// 	}
// 	else {
// 		if (find_file_by_fd(fd) != NULL) {
// 			write_result = file_write(find_file_by_fd(fd), buffer, size);
// 		}
// 		else {
// 			write_result = 0;
// 		}
// 	}
// 	lock_release(&filesys_lock);
// 	return write_result;
// }

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

// 파일 위치(offset)로 이동하는 함수
/* seek()은 열려있는 파일 fd에 쓰거나 읽을 바이트 위치를 인자로 넣어줄 position 위치로 변경하는 함수*/
/* 우리가 입력해줄 position 위치부터 읽을 수 있도록 해당 position을 찾는 함수 */
void seek(int fd, unsigned position) {
	struct file *seek_file = find_file_by_fd(fd);
	if (seek_file <= 2) {		// 초기값 2로 설정. 0: 표준 입력, 1: 표준 출력
		return;
	}
	seek_file->pos = position;
}

/* 열린 파일의 위치를 알려주는 시스템 콜 */
// 파일의 위치(offset)을 알려주는 함수
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

/* close()는 열린 파일을 닫는 시스템 콜이다. 파일을 닫고 fd를 제거한다. */
// 열린 파일을 닫는 시스템 콜. 파일을 닫고 fd제거
void close(int fd) {
	struct file *fileobj = find_file_by_fd(fd);
	if (fileobj == NULL) {
		return;
	}
	remove_file_from_fdt(fd);
}
