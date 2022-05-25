#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Thread destruction requests */
static struct list destruction_req;

/* sleep 상태의 스레드들을 저장하는 리스트 */
static struct list sleep_list;
/* ready list에서 맨 처음으로 awake할 스레드의 tick 값 */
static int64_t next_tick_to_awake;

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* Returns true if T appears to point to a valid thread. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/* Returns the running thread.
 * Read the CPU's stack pointer `rsp', and then round that
 * down to the start of a page.  Since `struct thread' is
 * always at the beginning of a page and the stack pointer is
 * somewhere in the middle, this locates the curent thread. */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// Global descriptor table for the thread_start.
// Because the gdt will be setup after the thread_init, we should
// setup temporal gdt first.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* Reload the temporal gdt for the kernel
	 * This gdt does not include the user context.
	 * The kernel will rebuild the gdt with user context, in gdt_init (). */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* Init the globla thread context */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&destruction_req);
	list_init (&sleep_list); // sleep 스레드들을 연결해놓은 리스트를 초기화한다.
	next_tick_to_awake = INT64_MAX;

	/* Set up a thread structure for the running thread. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/* idle 스레드를 만들고, preemptive thread scheduling을 시작한다. */
void
thread_start (void) {
	/* Create the idle thread. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	// idle 스레드를 만들고 맨 처음 ready queue에 들어간다.
	// semaphore를 1로 UP 시켜 공유 자원의 접근을 가능하게 한 다음 바로 BLOCK 된다.
	thread_create ("idle", PRI_MIN, idle, &idle_started); 

	/* Start preemptive thread scheduling. */
	// thread_create(idle)에서 disable 했던 인터럽트 상태를 enable로 만듬 -> create 안에 unblock에 disable이 있음
	// 이제 스케쥴링이 가능함 인터럽트가 가능해서
	intr_enable ();

	/* Wait for the idle thread to initialize idle_thread. */
	sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) {	
	struct thread *t = thread_current (); // thread 유효성 검사

	/* Update statistics. */
	if (t == idle_thread)	
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* Enforce preemption. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/* Prints thread statistics. */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */

// 스레드들의 우선순위에 따른 CPU 선점이 일어남
tid_t
thread_create (const char *name, int priority,
		thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* Allocate thread. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* Initialize thread. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* Call the kernel_thread if it scheduled.
	 * Note) rdi is 1st argument, and rsi is 2nd argument. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* Add to run queue. */
	thread_unblock (t);

	if (t->priority > thread_current()->priority) // 스레드를 맨 첨 만들고 unblock()시, 우선순위가 현재 CPU를 점유하고 있는 스레드보다 크다면 
		thread_yield(); // 양보한다

	return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	/*-----priority-------*/
	// list_push_back (&ready_list, &t->elem); // 깨우고 난 뒤 readylist의 맨 뒤 삽입
	list_insert_ordered(&ready_list, &t->elem, cmp_priority, NULL); // 우선 순위대로 정렬되어 삽입
	/*-----priority-------*/
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) {
	return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* Make sure T is really a thread.
	   If either of these assertions fire, then your thread may
	   have overflowed its stack.  Each thread has less than 4 kB
	   of stack, so a few big automatic arrays or moderate
	   recursion can cause stack overflow. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* Just set our status to dying and schedule another process.
	   We will be destroyed during the call to schedule_tail(). */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, &cmp_priority, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) {
	thread_current ()->init_priority = new_priority;

	refresh_priority();
	test_max_priority(); // 원래는 바꿔주고 끝이었음 우선순위를 바꾸고 난뒤 더 높은 우선순위를 가진 스레드가 있으면 양보해야하니까 이 함수를 호출
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE. */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: Your implementation goes here */
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) {
	/* TODO: Your implementation goes here */
	return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void // 어떤 스레드들도 실행되고 있지 않을 때 실행되는 스레드. 맨 처음 thread_start()가 호출될 때 ready queue에 먼저 들어가 있음
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* Let someone else run. */
		intr_disable ();
		thread_block ();

		/* Re-enable interrupts and wait for the next one.

		   The `sti' instruction disables interrupts until the
		   completion of the next instruction, so these two
		   instructions are executed atomically.  This atomicity is
		   important; otherwise, an interrupt could be handled
		   between re-enabling interrupts and waiting for the next
		   one to occur, wasting as much as one clock tick worth of
		   time.

		   See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
		   7.11.1 "HLT Instruction". */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* Function used as the basis for a kernel thread. */
static void
// 인자 - thread_func *function : kernel이 실행할 함수, void *aux : sync를 위한 semaphore 등
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* The scheduler runs with interrupts off. */
	function (aux);       /* Execute the thread function. */
	thread_exit ();       /* If function() returns, kill the thread. */
}


/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->magic = THREAD_MAGIC;

	/*----priority donation----*/
	t->init_priority = priority; // 스레드가 막 만들어졌으면, priority와 init_priority는 똑같이 인자 priority를 받음(원래 priority를 저장하기 위해)
	t->wait_on_lock = NULL; // wait_on_lock 역시 처음에 아무것도 없다. (신생 스레드가 lock을 요구한다고 해도, 실제 요구하는 시점은 running_thread가 되어서 처음으로 요구함 그때 갱신됨)
	list_init(&t->donations); // donations은 리스트형 구조이므로 리스트 초기화 함수를 사용함
	/*----priority donation----*/
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Use iretq to launch the thread */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* Switching the thread by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* The main switching logic.
	 * We first restore the whole execution context into the intr_frame
	 * and then switching to the next thread by calling do_iret.
	 * Note that, we SHOULD NOT use any stack from here
	 * until switching is done. */
	__asm __volatile (
			/* Store registers that will be used. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* Fetch input once */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // Saved rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // Saved rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // Saved rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // read the current rip.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* Schedules a new process. At entry, interrupts must be off.
 * This function modify current thread's status to status and then
 * finds another thread to run and switches to it.
 * It's not safe to call printf() in the schedule(). */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim =
			list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run (); // CPU 주도권을 넘겨줄 다음 스레드

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* Mark us as running. */
	next->status = THREAD_RUNNING; // 다음 스레드 상태 변경

	/* Start new time slice. */
	thread_ticks = 0;

#ifdef USERPROG
	/* Activate the new address space. */
	process_activate (next);
#endif

	if (curr != next) {
		/* If the thread we switched from is dying, destroy its struct
		   thread. This must happen late so that thread_exit() doesn't
		   pull out the rug under itself.
		   We just queuing the page free reqeust here because the page is
		   currently used bye the stack.
		   The real destruction logic will be called at the beginning of the
		   schedule(). */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* Before switching the thread, we first save the information
		 * of current running. */
		thread_launch (next); // 다음 스레드 실행
	}
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}

// 1) ticks = 자야할 시각(이 시각이 지나야 깨서도 running 상태로 갈 수 있음)
// 2) next_tick_to_awake = 스레드들 중에서 자야할 최소 시각
//    ex) 1,2,3 스레드 중에서 자야할 최소시각이 적은 스레드의 ticks값으로 갱신해준다
void update_next_tick_to_awake(int64_t ticks){
	next_tick_to_awake = (next_tick_to_awake>ticks) ? ticks : next_tick_to_awake;
} 

int64_t get_next_tick_to_awake(void) {
	return next_tick_to_awake;
}
/* 
	슬립 해줄 스레드를 sleep list에 추가하고 status를 blocked로 만들어준다
	이 때 idle thread를 sleep시켜준다면 CPU가 실행 상태를 유지할 수 없어 종료 되므로 예외처리를 해주어야함
*/
void thread_sleep(int64_t ticks){ //
	/* 현재 실행되고 있는 스레드에 대한 작업이므로. */
	struct thread* cur = thread_current();

	/* 인터럽트 disable*/
	enum intr_level old_level;
	ASSERT(!intr_context());
	old_level = intr_disable(); // 스레드를 list에 추가해주는 일은 인터럽트가 걸리면 안 된다.	

	ASSERT(cur != idle_thread);  // idle thread라면 종료.

	cur->wakeup_tick = ticks;						// wakeup_tick 업데이트 -> 시각 언제 깨울건가?
	update_next_tick_to_awake(cur->wakeup_tick); 	// next_tick_to_awake 업데이트(새로 들어온 것이 더 작은값인지 확인)
	list_push_back (&sleep_list, &cur->elem);		// sleep_list에 끝에 추가

	/* 스레드를 sleep 시킨다. */
	thread_block(); // 블락 상태로 상태바꿔주고 schedule도 해준다. -> 스케쥴을 함수를 스케줄링 전체라 생각하지마!

	/* 인터럽트 원복 */
	intr_set_level(old_level);
}

// sleep list에 잠자고 있는 스레드를 깨운다. 즉 sleep list에서 제거한 후 ready list에 넣어줌 status 역시 바꿔줌
void thread_awake(int64_t ticks){
	struct list_elem* cur = list_begin(&sleep_list); // 리스트의 처음 원소
	struct thread* t;


	/* sleep list의 끝까지 순환한다. */
	while(cur != list_end(&sleep_list)){ // list_end는 꼬리를 반환
		t = list_entry(cur, struct thread, elem); // list 원소를 스레드 구조체의 주소로 바꿔주고 포인터로 정의된 t에 주소값을 넣어준다

		if (ticks >= t->wakeup_tick){  // 깨울 시간이 지났다
			cur = list_remove(&t->elem); // 스레드 구조체의 elem를 제거하고 다음걸 cur에 넣어줌
			thread_unblock(t);           // status를 unblock상태로 바꿔준다.
		}
		else {  // 아직 안 깨워도 된다 : 다음 쓰레드로 넘어간다.
			cur = list_next(cur);
			update_next_tick_to_awake(t->wakeup_tick);  // next_tick이 바뀌었을 수 있으므로 업데이트해준다.
		}
	}
}

// 인자로 넣어준 스레드 a가 b보다 우선순위가 높다면 true를, 낮다면 false
bool cmp_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	struct thread *thread_a = list_entry(a, struct thread, elem);
	struct thread *thread_b = list_entry(b, struct thread, elem);
	return (thread_a->priority > thread_b->priority);
}

void test_max_priority(void){
	if (list_empty(&ready_list)) // 비어있으면 종료
		return;

	struct thread* max_priority = list_entry(list_front(&ready_list), struct thread, elem); // ready list 앞엔 정렬되어 들어가서 가장 높은우선순위 스레드가 있음

	if (max_priority->priority > thread_current()->priority){ // 레디리스트 맨앞에 있는 스레드의 우선순위가 더크면
		thread_yield(); // 현재 스레드는 양보해야한다.
	}
}

void donate_priority(){
	/*39 우선순위인 스레드가 C를 원하는데 C의 holder은 B를 원하고 B의 holder는 A를 원하는 경우, 이렇게 원하는 lock의 holder가 다른 lock을 원한다면 
	그 lock의 holder 까지 donation을 하는 것을 Nested Donation이라고 함 pintos는 nested depth를 8까지*/

	int depth;
	struct thread* curr = thread_current();

	/* 최대 depth는 8이다. */
	for (depth = 0; depth < 8; depth++){
		if (!curr->wait_on_lock)   // 더 이상 nested가 없을 때.
			break;
		
		struct thread* holder = curr->wait_on_lock->holder;
		holder->priority = curr->priority;   // 우선 순위를 donation한다.
		curr = holder;  //  그 다음 depth로 들어간다.
	}
}

void remove_with_lock(struct lock* lock){
	struct list_elem* e; // 기부자들 list 시작 점(락키 갖고 싶어서 도네이션 했음 그래서 도네이션 리스트에 있을거임)
	struct thread* curr = thread_current(); // 현재 러닝중인 thread(A,B 락키 가지고 있는 상황)

	for (e = list_begin(&curr->donations); e != list_end(&curr->donations); e = list_next(e)){
		struct thread* t = list_entry(e, struct thread, donation_elem);
		if (t->wait_on_lock == lock){ // 기부자가 기다리는 락이 인자로받은 락(지울 락)이랑 같으면
			list_remove(&t->donation_elem); // 그 elem은 삭제 해줘
		}
	}
}

void refresh_priority(void) {
	struct thread * curr = thread_current(); // lock_release()에서 remove_with_lock()로 락 해제해도 여전히 실행중인 스레드가 다른 락을 들고 있을 수 있으니 curr은 러닝중인 스레드로
	curr->priority = curr->init_priority; // 현재 스레드의 우선순위를 기부받기 전의 우선순위로 변경

	if (list_empty(&curr->donations) == false) { // 도네리스트 안비었으면
		list_sort(&curr->donations, thread_compare_donate_priority, 0); // 도네리스트 우선순위 비교해서 정렬해줘
		
		struct thread *high;
		high = list_entry(list_front(&curr->donations), struct thread, donation_elem); // 도네리스트에서 가장 우선순위 높은 elem
		
		if (high->priority > curr->priority) { // 러닝스레드 기부받기 전 우선순위보다 도네리스트에서 우선순위가 가장 높은애가 크면 -> 근데 당연해 왜냐면 도네리스트에 들어가는 전제 자체가 러닝 스레드 우선순위보다 높아야 들어가
			curr->priority = high->priority; // 러닝스레드 우선순위를 도네리스트에서 우선순위가 가장큰애로(기부)
		}
	}
}