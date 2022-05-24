/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
   */

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include <debug.h>
#include "threads/interrupt.h"
#include "threads/thread.h"


/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
   decrement it.

   - up or "V": increment the value (and wake up one waiting
   thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) {
	ASSERT (sema != NULL);

	sema->value = value;
	list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. This is
   sema_down function. 
   
   이 함수는 잠자기 상태일 수 있으므로 인터럽트 처리기 내에서 호출하면 안 됩니다. 
   이 함수는 인터럽트가 비활성화된 상태에서 호출될 수 있지만 
   잠자기 상태인 경우 다음 예약된 스레드가 인터럽트를 다시 켤 것입니다. 
   sema_down 함수입니다.
*/
void
// semaphore를 요청하고 획득했을 때(스레드가 임계영역에 들어갈 준비가 다 된경우 요청), 
// value 값을 1 낮춤
// 수정 : 우선순위대로 삽입되도록 수정
sema_down (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	ASSERT (!intr_context ());

	old_level = intr_disable (); 
	while (sema->value == 0) { // sema 값이 0이면 -> 임계영역에 다른 스레드가 존재
		// sema 요청한 thread가 임계영역에 접근하고 싶은데 이미 자리를 차지하고 있는 스레드가 있는 상황
		// 요청 thread가 running 상태에 도달(우선순위 높으면) 하지만 임계영역에 들어갈 수 있는 lock은 받지 못했음
      // lock을 요청하고 난 뒤, waiters에 들어가게 된다.
      // 1) list_push_back : waiters는 FIFO
      // 2) list_insert_ordered : waiters 우선 순위대로 삽입되도록 수정  
		// list_push_back (&sema->waiters, &thread_current ()->elem);
      list_insert_ordered(&sema->waiters, &thread_current ()->elem, cmp_priority, NULL);
		// waiters 리스트 삽입 시, 우선 순위대로 삽입되도록 수정
      thread_block ();
	}
	sema->value--; // sema 값이 1이었으면 바로 value 0으로 낮춘다.
	intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) {
	enum intr_level old_level;
	bool success;

	ASSERT (sema != NULL);

	old_level = intr_disable ();
	if (sema->value > 0)
	{
		sema->value--;
		success = true;
	}
	else
		success = false;
	intr_set_level (old_level);

	return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) {
	enum intr_level old_level;

	ASSERT (sema != NULL);
	old_level = intr_disable ();
	if (!list_empty (&sema->waiters)){
      list_sort(&sema->waiters, cmp_priority, NULL);
      // watier list에 있는 thread의 우선 순위가 변경 되었을 경우를 고려하여 watier list 정렬
		thread_unblock (list_entry (list_pop_front (&sema->waiters),
					struct thread, elem));
      // 우선 순위를 고려하여 들어갔으므로 맨 앞에 것을 unblock 시켜주면(내 우선순위순서로 ready list에 넣겠다)
   }
   sema->value++;
   test_max_priority();
   // priority preeption 기능 추가
	intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) {
	struct semaphore sema[2];
	int i;

	printf ("Testing semaphores...");
	sema_init (&sema[0], 0);
	sema_init (&sema[1], 0);
	thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
	for (i = 0; i < 10; i++)
	{
		sema_up (&sema[0]);
		sema_down (&sema[1]);
	}
	printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) {
	struct semaphore *sema = sema_;
	int i;

	for (i = 0; i < 10; i++)
	{
		sema_down (&sema[0]);
		sema_up (&sema[1]);
	}
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   LOCK을 초기화합니다. 잠금은 주어진 시간에 최대 단일 스레드에서 보유할 수 있습니다. 
   우리의 잠금은 "재귀적"이 아닙니다. 즉, 현재 잠금을 보유하고 있는 스레드가 해당 잠금을 획득하려고 시도하는 오류입니다.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. 
   
   잠금은 초기 값이 1인 세마포어의 특수화입니다. 
   잠금과 이러한 세마포어의 차이는 두 가지입니다. 
   첫째, 세마포어는 1보다 큰 값을 가질 수 있지만 잠금은 한 번에 단일 스레드에서만 소유할 수 있습니다. 
   둘째, 세마포에는 소유자가 없습니다. 
   즉, 한 스레드가 세마포를 "다운"한 다음 다른 스레드가 세마포를 "업"할 수 있지만 잠금을 사용하면 
   동일한 스레드가 세마포를 획득하고 해제해야 합니다. 이러한 제한 사항이 번거롭다면 잠금 대신 세마포어를 사용해야 한다는 좋은 신호입니다.
   */
void
lock_init (struct lock *lock) {
	ASSERT (lock != NULL);

	lock->holder = NULL;
	sema_init (&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   필요한 경우 사용할 수 있을 때까지 잠자기 상태에서 LOCK을 획득합니다. 
   잠금은 현재 스레드에서 이미 보유하고 있지 않아야 합니다.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. 
   
   이 함수는 휴면 상태일 수 있으므로 인터럽트 처리기 내에서 호출하면 안 됩니다. 
   이 함수는 인터럽트가 비활성화된 상태에서 호출될 수 있지만 잠자기 상태가 필요한 경우 인터럽트가 다시 켜집니다.
   */
void
lock_acquire (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (!lock_held_by_current_thread (lock)); //현재 스레드가 락을 유지하고 있으면 종료

	sema_down (&lock->semaphore); // sema_down으로 인해 임계 영역에 진입
	lock->holder = thread_current (); // 현재 Running 스레드인 것을 반환
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   LOCK 획득을 시도하고 성공하면 true를 반환하고 실패하면 false를 반환합니다. 
   잠금은 현재 스레드에서 이미 보유하고 있지 않아야 합니다.

   This function will not sleep, so it may be called within an
   interrupt handler. 
   
   이 함수는 잠자기 모드가 아니므로 인터럽트 처리기 내에서 호출될 수 있습니다.
   */
bool
lock_try_acquire (struct lock *lock) {
	bool success;

	ASSERT (lock != NULL);
	ASSERT (!lock_held_by_current_thread (lock));

	success = sema_try_down (&lock->semaphore);
	if (success)
		lock->holder = thread_current ();
	return success;
}

/* Releases LOCK, which must be owned by the current thread.
   This is lock_release function.
   
   현재 스레드가 소유해야 하는 LOCK을 해제합니다. lock_release 함수입니다

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. 
   
   인터럽트 핸들러는 잠금을 획득할 수 없으므로 인터럽트 핸들러 내에서 잠금 해제를 시도하는 것은 의미가 없습니다.
   */
void
lock_release (struct lock *lock) {
	ASSERT (lock != NULL);
	ASSERT (lock_held_by_current_thread (lock)); // 현재 스레드가 lock을 유지하고 않으면 종료

	lock->holder = NULL; // 현재 스레드의 상태를 NULL로 초기화
	sema_up (&lock->semaphore); // sema_up을 통해 임계 영역에서 벗어남
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) 
   
   현재 스레드가 LOCK을 유지하면 true를 반환하고 그렇지 않으면 false를 반환합니다. 
   (다른 스레드가 잠금을 보유하고 있는지 여부를 테스트하는 것은 성급할 수 있습니다.)
   */
bool
lock_held_by_current_thread (const struct lock *lock) {
	ASSERT (lock != NULL);

	return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem {
	struct list_elem elem;              /* List element. */
	struct semaphore semaphore;         /* This semaphore. */
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. 
   
   조건 변수 COND를 초기화합니다. 
   조건 변수를 사용하면 한 코드 조각이 조건에 신호를 보내고 협력 코드가 신호를 수신하고 
   이에 따라 작동할 수 있습니다.
   */


void
cond_init (struct condition *cond) {
	ASSERT (cond != NULL);

	list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   원자적으로 LOCK을 해제하고 COND가 다른 코드 조각에 의해 신호를 받을 때까지 기다립니다.
   COND가 신호를 받은 후 반환되기 전에 LOCK이 다시 획득됩니다. 
   이 함수를 호출하기 전에 LOCK을 유지해야 합니다.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   이 기능으로 구현된 모니터는 "Hoare" 스타일이 아닌 "Mesa" 스타일입니다. 
   즉, 신호 송수신이 원자적 연산이 아닙니다. 
   따라서 일반적으로 호출자는 대기가 완료된 후 조건을 다시 확인하고 필요한 경우 다시 기다려야 합니다.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   주어진 조건 변수는 단일 잠금에만 연결되지만 하나의 잠금은 여러 조건 변수와 연결될 수 있습니다. 
   즉, 잠금에서 조건 변수로의 일대다 매핑이 있습니다.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. 
   
   이 함수는 휴면 상태일 수 있으므로 인터럽트 처리기 내에서 호출하면 안 됩니다. 
   이 함수는 인터럽트가 비활성화된 상태에서 호출될 수 있지만 잠자기 상태가 필요한 경우 인터럽트가 다시 켜집니다.
   */
void
cond_wait (struct condition *cond, struct lock *lock) {
	struct semaphore_elem waiter;

	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock)); // 현재 스레드가 lock을 유지하지 않으면 종료

	sema_init (&waiter.semaphore, 0); // sema 초기화, 초기값 0
	// list_push_back (&cond->waiters, &waiter.elem); 
   // cond 변수의 watiers list에 waiter의 elem값을 넣어준다.
   waiter.semaphore.priority=thread_current()->priority;
   // 추가) waiter의 semaphore의 priority를 현재 실행중인 스레드의 priority값으로 받아준다->cmp 구조 간단해진다
   list_insert_ordered(&cond->waiters, &waiter.elem, cmp_sem_priority, NULL);
	lock_release (lock); // 슬립을 해야할 스레드는 락을 반납한다
	sema_down (&waiter.semaphore); // sema_down을 하면서(s=0) while문에 갇혀있는다. 그 다음 스레드가 신호를 주면(sema_up)(s=1) while문 탈출하여 s=0으로 한 뒤 반환 
	lock_acquire (lock); // 슬립에서 깨어난 스레드는 리턴하기 전에 락을 재획득한다.
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */

void
cond_signal (struct condition *cond, struct lock *lock UNUSED) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);
	ASSERT (!intr_context ());
	ASSERT (lock_held_by_current_thread (lock));


	if (!list_empty (&cond->waiters))
      // list_sort(&cond->waiters, cmp_sem_priority, NULL);
		sema_up (&list_entry (list_pop_front (&cond->waiters), struct semaphore_elem, elem)->semaphore);
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) {
	ASSERT (cond != NULL);
	ASSERT (lock != NULL);

	while (!list_empty (&cond->waiters))
		cond_signal (cond, lock);
}

bool
cmp_sem_priority(const struct list_elem *a, const struct list_elem *b, void *aux UNUSED){
   int i = 0;
   struct semaphore_elem *sema_a = list_entry(a, struct semaphore_elem, elem);
   struct semaphore_elem *sema_b = list_entry(b, struct semaphore_elem, elem);
   
   // struct list *waiter_a = &(sema_a->semaphore.waiters);
   // struct list *waiter_b = &(sema_b->semaphore.waiters);

   // struct thread *sema_A = list_entry(list_begin(waiter_a), struct thread, elem);
	// struct thread *sema_B = list_entry(list_begin(waiter_b), struct thread, elem);

   // printf("a : %d, b : %d\n\n\n", sema_A->priority,sema_B->priority);


   // return sema_A->priority > sema_B->priority;
   printf("a : %d, b : %d\n\n\n", sema_a->semaphore.priority,sema_b->semaphore.priority);
   
   //a값 : 새로 들어오는 priority, b값 : waiters에 가장 앞에 있는 값 
   return sema_a->semaphore.priority > sema_b->semaphore.priority;
   //semaphore의 priority를 직접 들고 다니도록 하여 바로 비교
}
	


