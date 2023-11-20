/* SPDX-License-Identifier: MIT */
/*
 * Authors: Grzegorz Milos
 *          Robert Kaiser
 *          Costin Lupu <costin.lupu@cs.pub.ro>
 *
 * Copyright (c) 2005, Intel Research Cambridge
 * Copyright (c) 2017, NEC Europe Ltd., NEC Corporation. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
/*
 * The scheduler is non-preemptive (cooperative), and schedules according
 * to Round Robin algorithm.
 */
#include <uk/plat/config.h>
#include <uk/plat/lcpu.h>
#include <uk/plat/memory.h>
#include <uk/plat/time.h>
#include <uk/sched_impl.h>
#include <uk/schedcoop.h>
#include <uk/essentials.h>

struct schedcoop {
	struct uk_sched sched;
	struct uk_thread_list run_queue;
	struct uk_thread_list sleep_queue;

	struct uk_thread idle;
	__nsec idle_return_time;
	__nsec ts_prev_switch;
};

static inline struct schedcoop *uksched2schedcoop(struct uk_sched *s)
{
	UK_ASSERT(s);

	return __containerof(s, struct schedcoop, sched);
}

// 这个应该是主要的调度函数？
static void schedcoop_schedule(struct uk_sched *s)
{
	struct schedcoop *c = uksched2schedcoop(s);
	struct uk_thread *prev, *next, *thread, *tmp;
	__snsec now, min_wakeup_time;
	unsigned long flags;
	static unsigned int thread_list_size_is_greater_than_2_once = 0;
	unsigned int thread_list_size = 0;

	if (unlikely(ukplat_lcpu_irqs_disabled()))
		UK_CRASH("Must not call %s with IRQs disabled\n", __func__);

	now = ukplat_monotonic_clock();
	prev = uk_thread_current();
	flags = ukplat_lcpu_save_irqf();

#if 0 //TODO
	if (in_callback)
		UK_CRASH("Must not call %s from a callback\n", __func__);
#endif

	/* Update execution time of current thread */
	/* WARNING: We assume here that scheduler `s` is only responsible for
	 *          the current logical CPU. Otherwise, we would have to store
	 *          the time of the last context switch per logical core.
	 */
	prev->exec_time += now - c->ts_prev_switch;	// 增加当前线程的运行时间
	c->ts_prev_switch = now;					// 更新最新一次的调度时间

	/* Examine all sleeping threads.
	 * Wake up expired ones and find the time when the next timeout expires.
	 */
	min_wakeup_time = 0;
	UK_TAILQ_FOREACH_SAFE(thread, &c->sleep_queue,
			      queue, tmp) {
		if (likely(thread->wakeup_time)) {
			if (thread->wakeup_time <= now)
				uk_thread_wake(thread);	// 在 sleep_queue 里挨个检查，如果当前时间超过了这个线程的唤醒时间，就唤醒它
			else if (!min_wakeup_time
				 || thread->wakeup_time < min_wakeup_time)
				min_wakeup_time = thread->wakeup_time;	// 找出 sleep_queue 中最小的唤醒时间
		}
	}

	next = UK_TAILQ_FIRST(&c->run_queue);	// 从 run_queue 里取出第一个，当作接下来要执行的线程
	if (next) {
		UK_ASSERT(next != prev);
		UK_ASSERT(uk_thread_is_runnable(next));
		UK_ASSERT(!uk_thread_is_exited(next));
		UK_TAILQ_REMOVE(&c->run_queue, next,
				queue);						// 如果能取出这个“next”，就把这个线程从 run_queue 中移除

		/* Put previous thread on the end of the list */
		if ((prev != &c->idle)
		    && uk_thread_is_runnable(prev)
		    && !uk_thread_is_exited(prev))
			UK_TAILQ_INSERT_TAIL(&c->run_queue, prev,
					     queue);			// 把刚刚执行完的这个线程放到 run_queue 的末尾
	} 
	else if (uk_thread_is_runnable(prev)
		   && !uk_thread_is_exited(prev)) {
		next = prev;						// 如果取不出这个“next”，就看看现在这个还能不能接着跑，能的话，“next”还是它
	} 
	else {
		/*
		 * Schedule idle thread that will halt the CPU
		 * We select the idle thread only if we do not have anything
		 * else to execute
		 */
		c->idle_return_time = min_wakeup_time;	// 把怠惰进程的返回时间设置为最小唤醒时间（到了这个时间就有线程可以跑了）
		next = &c->idle;					// 如果取不出这个“next”，现在这个也没法接着跑了，那就跑怠惰进程
	}

	if (next != prev) {
		/*
		 * Queueable is used to cover the case when during a
		 * context switch, the thread that is about to be
		 * evacuated is interrupted and woken up.
		 */
		uk_thread_set_queueable(prev);
		uk_thread_clear_queueable(next);
	}

	// ZZC
#ifdef CONFIG_LIBUKSCHEDCOOP_DEBUG
	uk_pr_warn("[unicontainer]UK_TAILQ_EMPTY(&c->run_queue) is %u\n", UK_TAILQ_EMPTY(&c->run_queue));
	uk_pr_warn("[unicontainer]UK_TAILQ_EMPTY(&c->sleep_queue) is %u\n", UK_TAILQ_EMPTY(&c->sleep_queue));

	uk_pr_warn("[unicontainer]prev->name is %s\n", prev->name);
	if(next){
		uk_pr_warn("[unicontainer]next->name is %s\n", next->name);
	}

	if(!UK_TAILQ_EMPTY(&c->run_queue)){
		uk_pr_warn("[unicontainer]run_queue is:");
		UK_TAILQ_FOREACH(thread, &c->run_queue, queue){
			uk_pr_warn(" %s", thread->name);
		}
		uk_pr_warn("\n");
	}

	if(!UK_TAILQ_EMPTY(&c->sleep_queue)){
		uk_pr_warn("[unicontainer]sleep_queue is:");
		UK_TAILQ_FOREACH(thread, &c->sleep_queue, queue){
			uk_pr_warn(" %s", thread->name);
		}
		uk_pr_warn("\n");
	}	

	uk_pr_warn("[unicontainer]thread_list is:");
	UK_TAILQ_FOREACH(thread, &s->thread_list, thread_list){
		uk_pr_warn(" %s", thread->name);
	}
	uk_pr_warn("\n");
#endif /* CONFIG_LIBUKSCHEDCOOP_DEBUG */

	// 最开始的时候尝试用 run_queue 和 sleep_queue 当作判断依据，但系统在初始化时 run_queue 和 sleep_queue 也都为空，用这个当作判断条件的话系统就跑不起来了……
	// 反而是挂起的时候这两个队列也不会都为空，idle 会和 init 进程交替执行
	// 所以尝试用 thread_list 作为判断依据
	thread_list_size = 0;
	UK_TAILQ_FOREACH(thread, &s->thread_list, thread_list){
		thread_list_size++;
	}	

	if (thread_list_size_is_greater_than_2_once == 0 && thread_list_size > 2)
	{
		thread_list_size_is_greater_than_2_once = 1;	// thread_list 第一次有多于2个成员, 就把 thread_list_size_is_greater_than_2_once 设置为1
	}

	if (thread_list_size <= 2 && thread_list_size_is_greater_than_2_once == 1)
	{
		// thread_list_size_is_greater_than_2_once 为1，说明 thread_list 的成员数量有过大于2的时候，现在变回2了，说明用户程序执行完了
		UK_CRASH("[unicontainer]schedcoop_schedule() exit\n");
	}
	// 本思路进行了简单尝试，是成功的
	
	// ZZC-end

	ukplat_lcpu_restore_irqf(flags);

	/* Interrupting the switch is equivalent to having the next thread
	 * interrupted at the return instruction. And therefore at safe point.
	 */
	if (prev != next)
		uk_sched_thread_switch(next);	// 如果当前线程和接下来要跑的线程不是同一个，就 switch
}

static int schedcoop_thread_add(struct uk_sched *s, struct uk_thread *t)
{
	struct schedcoop *c = uksched2schedcoop(s);

	UK_ASSERT(t);
	UK_ASSERT(!uk_thread_is_exited(t));

	/* Add to run queue if runnable */
	if (uk_thread_is_runnable(t))
		UK_TAILQ_INSERT_TAIL(&c->run_queue, t, queue);	// 把要添加的线程加入到 run_queue 中

	return 0;
}

static void schedcoop_thread_remove(struct uk_sched *s, struct uk_thread *t)
{
	struct schedcoop *c = uksched2schedcoop(s);

	/* Remove from run_queue */
	if (t != uk_thread_current()
	    && uk_thread_is_runnable(t))
		UK_TAILQ_REMOVE(&c->run_queue, t, queue);
}

static void schedcoop_thread_blocked(struct uk_sched *s, struct uk_thread *t)
{
	struct schedcoop *c = uksched2schedcoop(s);

	UK_ASSERT(ukplat_lcpu_irqs_disabled());

	if (t != uk_thread_current())
		UK_TAILQ_REMOVE(&c->run_queue, t, queue);
	if (t->wakeup_time > 0)
		UK_TAILQ_INSERT_TAIL(&c->sleep_queue, t, queue);
}

static void schedcoop_thread_woken(struct uk_sched *s, struct uk_thread *t)
{
	struct schedcoop *c = uksched2schedcoop(s);

	UK_ASSERT(ukplat_lcpu_irqs_disabled());

	if (t->wakeup_time > 0)
		UK_TAILQ_REMOVE(&c->sleep_queue, t, queue);
	if (uk_thread_is_queueable(t) && uk_thread_is_runnable(t)) {
		UK_TAILQ_INSERT_TAIL(&c->run_queue, t, queue);
		uk_thread_clear_queueable(t);
	}
}

// 这个就是 idle 进程？
// 这个函数只会在最开始的时候运行一次，为什么呢？
static __noreturn void idle_thread_fn(void *argp)
{
	struct schedcoop *c = (struct schedcoop *) argp;
	__nsec now, wake_up_time;
	unsigned long flags;

	UK_ASSERT(c);

	// ZZC
	uk_pr_warn("[unicontainer]idle_thread_fn()\n");
	uk_pr_warn("[unicontainer]UK_TAILQ_EMPTY(&c->run_queue) is %u\n", UK_TAILQ_EMPTY(&c->run_queue));
	uk_pr_warn("[unicontainer]UK_TAILQ_EMPTY(&c->sleep_queue) is %u\n", UK_TAILQ_EMPTY(&c->sleep_queue));
	// ZZC-end

	for (;;) {
		flags = ukplat_lcpu_save_irqf();

		/*
		 * FIXME: We assume that `uk_sched_thread_gc()` is non-blocking
		 *        because we implement a cooperative scheduler. However,
		 *        this assumption may not be true depending on the
		 *        destructor functions that are assigned to the threads
		 *        and are called by `uk_sched_thred_gc()`.
		 *	  Also check if in the meantime we got a runnable
		 *	  thread.
		 * NOTE:  This idle thread must be non-blocking so that the
		 *        scheduler has always something to schedule.
		 */
		if (uk_sched_thread_gc(&c->sched) > 0 ||
		    UK_TAILQ_FIRST(&c->run_queue)) {
			/* We collected successfully some garbage or there is
			 * a runnable thread in the queue.
			 * Check if something else can be scheduled now.
			 */
			ukplat_lcpu_restore_irqf(flags);
			schedcoop_schedule(&c->sched);

			continue;
		}

		/* Read return time set by last schedule operation */
		wake_up_time = (volatile __nsec) c->idle_return_time;
		now = ukplat_monotonic_clock();

		if (!wake_up_time || wake_up_time > now) {
			if (wake_up_time)
				ukplat_lcpu_halt_irq_until(wake_up_time);
			else
				ukplat_lcpu_halt_irq();

			/* handle pending events if any */
			ukplat_lcpu_irqs_handle_pending();
		}

		ukplat_lcpu_restore_irqf(flags);

		/* try to schedule a thread that might now be available */
		schedcoop_schedule(&c->sched);
	}
}

static int schedcoop_start(struct uk_sched *s,
			   struct uk_thread *main_thread __maybe_unused)
{
	struct schedcoop *c = uksched2schedcoop(s);

	UK_ASSERT(main_thread);
	UK_ASSERT(main_thread->sched == s);
	UK_ASSERT(uk_thread_is_runnable(main_thread));
	UK_ASSERT(!uk_thread_is_exited(main_thread));
	UK_ASSERT(uk_thread_current() == main_thread);

	/* Since we are now starting to schedule, we save the current timestamp
	 * as the start time for the first time slice.
	 */
	c->ts_prev_switch = ukplat_monotonic_clock();

	/* NOTE: We do not put `main_thread` into the thread list.
	 *       Current running threads will be added as soon as
	 *       a different thread is scheduled.
	 */

	ukplat_lcpu_enable_irq();

	return 0;
}

static const struct uk_thread *schedcoop_idle_thread(struct uk_sched *s,
						     unsigned int proc_id)
{
	struct schedcoop *c = uksched2schedcoop(s);

	/* NOTE: We only support one processing LCPU (for now) */
	if (proc_id > 0)
		return NULL;

	return &(c->idle);
}

struct uk_sched *uk_schedcoop_create(struct uk_alloc *a)
{
	struct schedcoop *c = NULL;
	int rc;

	uk_pr_info("Initializing cooperative scheduler\n");
	c = uk_zalloc(a, sizeof(struct schedcoop));
	if (!c)
		goto err_out;

	UK_TAILQ_INIT(&c->run_queue);
	UK_TAILQ_INIT(&c->sleep_queue);

	/* Create idle thread */
	rc = uk_thread_init_fn1(&c->idle,
				idle_thread_fn, (void *) c,
				a, STACK_SIZE,
				a, false,
				NULL,
				"idle",
				NULL,
				NULL);
	if (rc < 0)
		goto err_free_c;

	c->idle.sched = &c->sched;

	uk_sched_init(&c->sched,
			schedcoop_start,		// sched_start
			schedcoop_schedule,		// yield
			schedcoop_thread_add,	// thread_add
			schedcoop_thread_remove,	// thread_remove
			schedcoop_thread_blocked,	// thread_blocked
			schedcoop_thread_woken,
			schedcoop_idle_thread,
			a);		// 初始化这个调度器（给调度器的各个函数指针赋值）

	/* Add idle thread to the scheduler's thread list */
	UK_TAILQ_INSERT_TAIL(&c->sched.thread_list, &c->idle, thread_list);

	return &c->sched;

err_free_c:
	uk_free(a, c);
err_out:
	return NULL;
}
