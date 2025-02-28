/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * Authors: Simon Kuenzer <simon.kuenzer@neclab.eu>
 *
 * Copyright (c) 2022, NEC Laboratories Europe GmbH, NEC Corporation.
 *                     All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <uk/process.h>
#include <uk/print.h>
#include <uk/syscall.h>
#include <uk/arch/limits.h>
#include <uk/semaphore.h>
#include <uk/sched.h>

#include "process.h"

/* Up to cl_args->tls, the fields of clone_args are required arguments */
#define CL_ARGS_REQUIRED_LEN					\
	(__offsetof(struct clone_args, tls)			\
	 + sizeof(((struct clone_args *)0)->tls))

extern const struct uk_posix_clonetab_entry _uk_posix_clonetab_start[];
extern const struct uk_posix_clonetab_entry _uk_posix_clonetab_end;

static __uk_tls struct {
	bool is_cloned;
	__u64 cl_flags;
} cl_status = { false, 0x0 };

#ifdef CONFIG_LIBUKDEBUG_ENABLE_ASSERT
#define CL_UKTLS_SANITY_MAGIC 0xb0b0f00d /* Bobo food */
static __thread uint32_t cl_uktls_magic = CL_UKTLS_SANITY_MAGIC;
#endif /* CONFIG_LIBUKDEBUG_ENABLE_ASSERT */

#define uk_posix_clonetab_foreach(itr)					\
	for ((itr) = DECONST(struct uk_posix_clonetab_entry*,		\
			     _uk_posix_clonetab_start);			\
	     (itr) < &(_uk_posix_clonetab_end);				\
	     (itr)++)

#define uk_posix_clonetab_foreach_reverse2(itr, start)			\
	for ((itr) = (start);						\
	     (itr) >= _uk_posix_clonetab_start;				\
	     (itr)--)

#define uk_posix_clonetab_foreach_reverse(itr)				\
	uk_posix_clonetab_foreach_reverse2((itr),			\
			(DECONST(struct uk_posix_clonetab_entry*,	\
				 (&_uk_posix_clonetab_end))) - 1)

struct _clonetab_init_call_args {
	uk_posix_clone_init_func_t init;
	const struct clone_args *cl_args;
	size_t cl_args_len;
	__u64 cl_flags_optional;
	struct uk_thread *child;
	struct uk_thread *parent;
};

static int _clonetab_init_call(void *argp)
{
	struct _clonetab_init_call_args *args
		= (struct _clonetab_init_call_args *) argp;
	int ret;

	UK_ASSERT(args);
	UK_ASSERT(args->init);

	ret = (args->init)(args->cl_args, args->cl_args_len,
			   args->child, args->parent);
	return ret;
}
struct _clonetab_term_call_args {
	uk_posix_clone_term_func_t term;
	__u64 cl_flags;
	struct uk_thread *child;
};

static int _clonetab_term_call(void *argp)
{
	struct _clonetab_term_call_args *args
		= (struct _clonetab_term_call_args *) argp;

	UK_ASSERT(args);
	UK_ASSERT(args->term);

	(args->term)(args->cl_flags, args->child);
	return 0;
}
/** Iterates over registered thread initialization functions
 * 	遍历已注册的线程初始化函数
 */
static int _uk_posix_clonetab_init(const struct clone_args *cl_args,
				   size_t cl_args_len,
				   __u64 cl_flags_optional,
				   struct uk_thread *child,
				   struct uk_thread *parent)
{
	struct uk_posix_clonetab_entry *itr;
	struct _clonetab_init_call_args init_args;
	struct _clonetab_term_call_args term_args;
	int ret = 0;
	__u64 flags;

	UK_ASSERT(cl_args);
	UK_ASSERT(cl_args_len >= CL_ARGS_REQUIRED_LEN);
	UK_ASSERT(child);
	UK_ASSERT(parent);

	/* Test if we can handle all requested clone flags.
	 * In case we fail, we do not need to re-wind the operations like as
	 * we would call the init functions already.
	 * 
	 * 这个 clone table 里存放的是用 UK_POSIX_CLONE_HANDLER 宏注册的对各个 flag 的处理函数，
	 * 调试中的单个项如下所示：
	 * (gdb) print *_uk_posix_clonetab_start
	 * $4 = {flags_mask = 65536, presence_only = false, init = 0x126790 <pprocess_clone_thread>, term = 0x0}
	 */
	flags = cl_args->flags;
	uk_posix_clonetab_foreach(itr) {	// 这个 itr 可能和 for 循环里的 i 类似
		if (unlikely(!itr->init))
			continue;

		/* Masked out flags that we can handle */
		flags &= ~itr->flags_mask;	// 去掉所有支持的 flag，然后在下面检查是否有不支持的 flag 留着？重点似乎是这个itr
	}
	/* Mask out optional flags:
	 * We should not fail if we can't handle those
	 */
	flags &= ~cl_flags_optional;

	if (flags != 0x0) {
		uk_pr_warn("posix_clone %p (%s): Unsupported clone flags requested: 0x%"__PRIx64"\n",
			   child, child->name ? child->name : "<unnamed>",
			   flags);
		ret = -ENOTSUP;
		goto out;
	}

	/* Call handlers according to clone flags */
	init_args.cl_args     = cl_args;
	init_args.cl_args_len = cl_args_len;
	init_args.child       = child;
	init_args.parent      = parent;
	uk_posix_clonetab_foreach(itr) {
		if (unlikely(!itr->init))
			continue;
		if (itr->presence_only && !(cl_args->flags & itr->flags_mask))
			continue;

		uk_pr_debug("posix_clone %p (%s) init: Call initialization %p() [flags: 0x%"__PRIx64"]...\n",
			    child, child->name ? child->name : "<unnamed>",
			    *itr->init, itr->flags_mask);

		/* NOTE: We call the init function with the Unikraft TLS of the
		 *       created child in order to enable TLS initializations.
		 */
		init_args.init = *itr->init;
		ret = ukplat_tlsp_exec(child->uktlsp, _clonetab_init_call,
				       &init_args);		// 这里调用对各个 flag 的初始化处理函数
		if (ret < 0) {
			uk_pr_debug("posix_clone %p (%s) init: %p() returned %d\n",
				    child,
				    child->name ? child->name : "<unnamed>",
				    *itr->init, ret);
			goto err;
		}
	}

	/* Set status in the child (TLS variable) */
	uk_thread_uktls_var(child, cl_status.is_cloned) = true;
	uk_thread_uktls_var(child, cl_status.cl_flags)  = cl_args->flags;
	ret = 0; /* success */
	goto out;

err:
	/* Run termination functions starting from one level before the failed
	 * one for cleanup (also with child TLS context)
	 */
	term_args.cl_flags = cl_args->flags;
	term_args.child   = child;
	uk_posix_clonetab_foreach_reverse2(itr, itr - 2) {
		if (unlikely(!itr->term))
			continue;
		if (itr->presence_only && !(cl_args->flags & itr->flags_mask))
			continue;

		uk_pr_debug("posix_clone %p (%s) init: Call termination %p() [flags: 0x%"__PRIx64"]...\n",
			    child, child->name ? child->name : "<unnamed>",
			    *itr->term, itr->flags_mask);
		term_args.term = itr->term;
		ukplat_tlsp_exec(child->uktlsp, _clonetab_term_call,
				 &term_args);
	}
out:
	return ret;
}

/** Iterates over registered clone termination functions for threads that
 *  were created with clone
 * NOTE: This function is called from child TLS context
 */
static void uk_posix_clonetab_term(struct uk_thread *child)
{
	struct uk_posix_clonetab_entry *itr;

	UK_ASSERT(child);
	UK_ASSERT(ukplat_tlsp_get() == child->uktlsp);

	/* Only if this thread was cloned, call the clone termination callbacks
	 */
	if (!cl_status.is_cloned)
		return;

	/* Go over clone termination functions that match with
	 * child's ECTX and UKTLS feature requirements
	 */
	uk_posix_clonetab_foreach_reverse(itr) {
		if (unlikely(!itr->term))
			continue;
		if (itr->presence_only &&
		    !(cl_status.cl_flags & itr->flags_mask))
			continue;

		uk_pr_debug("posix_clone %p (%s) term: Call termination %p() [flags: 0x%"__PRIx64"]...\n",
			    child, child->name ? child->name : "<unnamed>",
			    *itr->term, itr->flags_mask);
		(itr->term)(cl_status.cl_flags, child);
	}

	cl_status.is_cloned = false;
	cl_status.cl_flags = 0x0;
}
UK_THREAD_INIT_PRIO(0x0, uk_posix_clonetab_term, UK_PRIO_LATEST);

/*
 * NOTE: From man pages about clone(2)
 *       (https://man7.org/linux/man-pages/man2/clone.2.html):
 *       "The raw clone() system call corresponds more closely to fork(2)
 *        in that execution in the child continues from the point of the
 *        call.  As such, the fn and arg arguments of the clone() wrapper
 *        function are omitted.
 *
 *        In contrast to the glibc wrapper, the raw clone() system call
 *        accepts NULL as a stack argument (and clone3() likewise allows
 *        cl_args.stack to be NULL).  In this case, the child uses a
 *        duplicate of the parent's stack.  (Copy-on-write semantics ensure
 *        that the child gets separate copies of stack pages when either
 *        process modifies the stack.)  In this case, for correct
 *        operation, the CLONE_VM option should not be specified.  (If the
 *        child shares the parent's memory because of the use of the
 *        CLONE_VM flag, then no copy-on-write duplication occurs and chaos
 *        is likely to result.)
 *
 *        The order of the arguments also differs in the raw system call,
 *        and there are variations in the arguments across architectures,
 *        as detailed in the following paragraphs.
 *
 *        The raw system call interface on x86-64 and some other
 *        architectures (including sh, tile, and alpha) is:
 *
 *            long clone(unsigned long flags, void *stack,
 *                       int *parent_tid, int *child_tid,
 *                       unsigned long tls);
 *
 *        On x86-32, and several other common architectures (including
 *        score, ARM, ARM 64, PA-RISC, arc, Power PC, xtensa, and MIPS),
 *        the order of the last two arguments is reversed:
 *
 *            long clone(unsigned long flags, void *stack,
 *                       int *parent_tid, unsigned long tls,
 *                       int *child_tid);
 *       "
 */
static void _clone_child_gc(struct uk_thread *t)
{
	if (t->name) {
		free(DECONST(char *, t->name));
		t->name = NULL;
	}
}

/*
 * NOTE: The clone system call and the handling of the TLS
 *
 *       `_clone()` assumes that a passed TLS pointer is an Unikraft TLS.
 *       The only exception exists if `_clone()` is called from a context
 *       where a custom TLS is already active (depends on
 *       `CONFIG_LIBSYSCALL_SHIM_HANDLER_ULTLS`). In such a case, an
 *       Unikraft TLS is allocated but the passed TLS pointer is activated.
 *       The reason is that Unikraft libraries place TLS variables and use
 *       the TLS effectively as TCB.
 *       In case no TLS is handed over (CLONE_SETTLS is not set), _clone will
 *       still allocate an Unikraft TLS but sets the TLS architecture pointer
 *       to zero.
 * 
 * return_addr 保存的是用户程序中 clone syscall 之后的那个指令地址
 */
static int _clone(struct clone_args *cl_args, size_t cl_args_len,
		  __uptr return_addr)
{
	struct uk_thread *t;
	struct uk_sched *s;
	struct uk_thread *child = NULL;
	__u64 flags;
	int ret;

	t = uk_thread_current();	// 获取当前线程
	s = uk_sched_current();		// 获取当前调度器
  
	UK_ASSERT(s);
	UK_ASSERT(t);
	/* Parent must have ECTX and a Unikraft TLS */
	UK_ASSERT((t->flags & UK_THREADF_ECTX)
		  && (t->flags & UK_THREADF_UKTLS));
	UK_ASSERT(return_addr);

	if (!cl_args || cl_args_len < CL_ARGS_REQUIRED_LEN) {
		uk_pr_debug("No or invalid clone arguments given\n");
		ret = -EINVAL;
		goto err_out;
	}

	flags = cl_args->flags;

#if UK_DEBUG
	uk_pr_debug("uk_syscall_r_clone(\n");
	uk_pr_debug(" flags: 0x%lx [", flags);
	if (flags & CLONE_NEWTIME)		uk_pr_debug(" NEWTIME");
	if (flags & CLONE_VM)			uk_pr_debug(" VM");
	if (flags & CLONE_FS)			uk_pr_debug(" FS");
	if (flags & CLONE_FILES)		uk_pr_debug(" FILES");
	if (flags & CLONE_SIGHAND)		uk_pr_debug(" SIGHAND");
	if (flags & CLONE_PIDFD)		uk_pr_debug(" PIDFD");
	if (flags & CLONE_PTRACE)		uk_pr_debug(" PTRACE");
	if (flags & CLONE_VFORK)		uk_pr_debug(" VFORK");
	if (flags & CLONE_PARENT)		uk_pr_debug(" PARENT");
	if (flags & CLONE_THREAD)		uk_pr_debug(" THREAD");
	if (flags & CLONE_NEWNS)		uk_pr_debug(" NEWNS");
	if (flags & CLONE_SYSVSEM)		uk_pr_debug(" SYSVSEM");
	if (flags & CLONE_SETTLS)		uk_pr_debug(" SETTLS");
	if (flags & CLONE_PARENT_SETTID)	uk_pr_debug(" PARENT_SETTID");
	if (flags & CLONE_CHILD_CLEARTID)	uk_pr_debug(" CHILD_CLEARTID");
	if (flags & CLONE_DETACHED)		uk_pr_debug(" DETACHED");
	if (flags & CLONE_UNTRACED)		uk_pr_debug(" UNTRACED");
	if (flags & CLONE_CHILD_SETTID)		uk_pr_debug(" CHILD_SETTID");
	if (flags & CLONE_NEWCGROUP)		uk_pr_debug(" NEWCGROUP");
	if (flags & CLONE_NEWUTS)		uk_pr_debug(" NEWUTS");
	if (flags & CLONE_NEWIPC)		uk_pr_debug(" NEWIPC");
	if (flags & CLONE_NEWUSER)		uk_pr_debug(" NEWUSER");
	if (flags & CLONE_NEWPID)		uk_pr_debug(" NEWPID");
	if (flags & CLONE_NEWNET)		uk_pr_debug(" NEWNET");
	if (flags & CLONE_IO)			uk_pr_debug(" IO");
	uk_pr_debug(" ]\n");
	if (flags & CLONE_PIDFD)
		uk_pr_debug(" pidfd: %d\n", (int) cl_args->pidfd);
	if (flags & CLONE_PARENT_SETTID)
		uk_pr_debug(" parent_tid: %p\n", (void *) cl_args->parent_tid);
	if (flags & (CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID))
		uk_pr_debug(" child_tid: %p\n", (void *) cl_args->child_tid);
	uk_pr_debug(" stack: %p\n", (void *) cl_args->stack);
	uk_pr_debug(" tls: %p\n", (void *) cl_args->tls);
	uk_pr_debug(" <return>: %p\n", (void *) return_addr);
	uk_pr_debug(")\n");
#endif /* UK_DEBUG */

	if ((flags & CLONE_SETTLS)
#if CONFIG_LIBSYSCALL_SHIM_HANDLER_ULTLS
	    && (uk_syscall_ultlsp() == 0x0)
#endif /* CONFIG_LIBSYSCALL_SHIM_HANDLER_ULTLS */
	) {
		/* The caller already created a TLS for the child (for instance
		 * by a pthread API wrapper). We expect that this TLS is a
		 * Unikraft TLS.
		 */
		uk_pr_debug("Using passed TLS pointer %p as an Unikraft TLS\n",
			    (void *) cl_args->tls);
		child = uk_thread_create_container2(s->a,
						    (__uptr) cl_args->stack,
						    (__uptr) cl_args->tls,
						    true, /* TLS is an UKTLS */
						    false, /* We want ECTX */
						    (t->name) ? strdup(t->name)
							      : NULL,
						    NULL,
						    _clone_child_gc);
	} else {
		/* If no TLS was given or the parent calls us already from
		 * a context with an userland TLS activated (kernel land vs.
		 * user land), we allocate an Unikraft TLS because Unikraft
		 * places TLS variables and uses them effectively as TCB.
		 */
#if CONFIG_LIBSYSCALL_SHIM_HANDLER_ULTLS
		if (uk_syscall_ultlsp() != 0x0) {
			uk_pr_debug("Allocating an Unikraft TLS for the new child, parent called from context with custom TLS\n");
		} else
#endif /* CONFIG_LIBSYSCALL_SHIM_HANDLER_ULTLS */
		{
			uk_pr_debug("Allocating an Unikraft TLS for the new child, no TLS given by parent\n");
		}
		child = uk_thread_create_container(s->a,
						   NULL, 0, /* Stack is given */
						   s->a_uktls,
						   false, /* We want ECTX */
						   (t->name) ? strdup(t->name)
							     : NULL,
						   NULL,
						   _clone_child_gc);
	}
	if (PTRISERR(child)) {
		ret = (PTR2ERR(child) != 0) ? PTR2ERR(child) : -ENOMEM;
		goto err_out;
	}
#ifdef CONFIG_LIBUKDEBUG_ENABLE_ASSERT
	/* Sanity check that the UKTLS of the child is really a Unikraft TLS:
	 * Do we find our magic on the TLS, is Bobo's banana there?
	 */
	UK_ASSERT(uk_thread_uktls_var(child, cl_uktls_magic)
		  == CL_UKTLS_SANITY_MAGIC);
#endif /* CONFIG_LIBUKDEBUG_ENABLE_ASSERT */

	/* CLONE_SETTLS: Instead of just activating the Unikraft TLS, we
	 * activate the passed TLS pointer as soon as the child wakes up.
	 * NOTE: If SETTLS is not set, we do not activate any TLS although
	 *       an Unikraft TLS was allocated.
	 */
	child->tlsp = (flags & CLONE_SETTLS) ? cl_args->tls : 0x0;	// 设置 TLS pointer？这里为什么给 child 分配好了 tlsp 然后又赋值成 cl_args->tls？

#ifdef CONFIG_LIBEXECHOOK
	/** 不管三七二十一，把 child->tlsp 就设置成 child->uktlsp;
	 * 那还需要强制加 CLONE_SETTLS 吗？ */
	UK_ASSERT(child->uktlsp);
	child->tlsp = child->uktlsp;
#endif /* CONFIG_LIBEXECHOOK */

	uk_pr_debug("Child is going to wake up with TLS pointer set to: %p (%s TLS)\n",
		    (void *) child->tlsp,
		    (child->tlsp != child->uktlsp) ? "custom" : "Unikraft");

	/* Call clone handler table but treat CLONE_SETTLS as handled */
	ret = _uk_posix_clonetab_init(cl_args, cl_args_len,
				      CLONE_SETTLS,
				      child, t);
	if (ret < 0)
		goto err_free_child;
	uk_pr_debug("Thread cloned %p (%s) -> %p (%s): %d\n",
		    t, t->name ? child->name : "<unnamed>",
		    child, child->name ? child->name : "<unnamed>", ret);

	/*
	 * Child starts at return address, sets given stack and given TLS.
	 * Register clearing has the effect that it looks like `clone`
	 * returns `0` in the child.
	 */
	ukarch_ctx_init(&child->ctx,
			(__uptr) cl_args->stack,
			false,
			return_addr);
	uk_thread_set_runnable(child);	// 把这个新的 child 设置为可运行

	/* We will return the child's thread ID in the parent */
	ret = ukthread2tid(child);

	/* Assign the child to the scheduler */
	uk_sched_thread_add(s, child);	// 把这个新的 child 加入 s->thread_list（s 是调度器）

	// ZZC: CLONE_VFORK 支持
	child->parent = t;
	uk_pr_debug("[unicontainer]_clone() debug0\n");
	if (flags & CLONE_VFORK){
		if(!t->vfork_sem){
			t->vfork_sem = (struct uk_semaphore*)uk_malloc(s->a, sizeof(struct uk_semaphore));	// 如果没初始化过，就给父进程的 vfork_sem 分配空间，初始化为1
			uk_semaphore_init(t->vfork_sem, 0);
		}
		UK_ASSERT(t->vfork_sem);

		child->vfork_sem = (struct uk_semaphore*)uk_malloc(s->a, sizeof(struct uk_semaphore));	// 初始化子进程的 vfork_sem
		uk_semaphore_init(child->vfork_sem, 0);
		UK_ASSERT(child->vfork_sem);
		
		uk_pr_debug("[unicontainer]_clone() debug2\n");

		uk_semaphore_down(t->vfork_sem);		// 父进程的 vfork 信号量减一；这里是不是应该重新做一次调度了？
	}
	uk_pr_debug("[unicontainer]_clone() debug3\n");
	// ZZC-end

	return ret;

err_free_child:
	uk_thread_release(child);
err_out:
	return ret;
}

#if CONFIG_ARCH_X86_64
UK_LLSYSCALL_R_DEFINE(int, clone,
		      unsigned long, flags,
		      void *, sp,
		      int *, parent_tid,
		      int *, child_tid,
		      unsigned long, tlsp)
#else /* !CONFIG_ARCH_X86_64 */
UK_LLSYSCALL_R_DEFINE(int, clone,
		      unsigned long, flags,
		      void *, sp,
		      int *, parent_tid,
		      unsigned long, tlsp,
		      int *, child_tid)
#endif /* !CONFIG_ARCH_X86_64 */
{
#ifdef CONFIG_LIBEXECHOOK
	/** ZZC:
	 * 如果 clone 的 flag 只有 CLONE_VM | CLONE_VFORK，那一般接下来就要做 execve 了，这种情况就是我们的 exec_hook 需要处理的；
	 * 但 unikraft 又要求 clone 时必须有 CLONE_FS | CLONE_FILES，所以额外加上；
	 * 如果不加 CLONE_SETTLS 跑很多标准库函数时会报错。
	 * 0x11 是 SIGCHLD。
	 * 
	 * 另外也有用 CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID 作为 flag 来完成 clone + execve 的情况,
	 * 这里做了一点点处理,但不完善。
	 * */
	if((flags == (CLONE_VM | CLONE_VFORK)) || (flags == (CLONE_VM | CLONE_VFORK | 0x11)) || (flags == (CLONE_CHILD_CLEARTID | CLONE_CHILD_SETTID | 0x11))){
		flags |= CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SETTLS;
	}
	if(!sp){
		// 这一段暂时没啥用
		sp = (void*)malloc(1024 * 1024);
	}
#endif /* !CONFIG_LIBEXECHOOK */

	/* Translate */
	struct clone_args cl_args = {
		.flags       = (__u64) (flags & ~0xff),
		.pidfd       = (__u64) ((flags & CLONE_PIDFD) ? parent_tid : 0),
		.child_tid   = (__u64) child_tid,
		.parent_tid  = (__u64) ((flags & CLONE_PIDFD) ? 0 : parent_tid),
		.exit_signal = (__u64) (flags & 0xff),
		.stack       = (__u64) sp,
		.tls         = (__u64) tlsp
	};

	return _clone(&cl_args, sizeof(cl_args), uk_syscall_return_addr());
}

#if UK_LIBC_SYSCALLS
int clone(int (*fn)(void *) __unused, void *sp __unused,
	  int flags __unused, void *arg __unused,
	  ... /* pid_t *parent_tid, void *tls, pid_t *child_tid */)
{
	/* TODO */
	errno = EINVAL;
	return -1;
}
#endif /* UK_LIBC_SYSCALLS */

/*
 * Checks that the CLONE_VM is set so that we make sure that
 * the address space is shared. Unikraft does currently not support
 * multiple application address spaces.
 */
// 乍一眼看过去还以为是 check kvm，寻思这检查些什么啊…… 再一看，原来是 check vm（CLONE_VM）
static int uk_posix_clone_checkvm(const struct clone_args *cl_args,
				  size_t cl_args_len __unused,
				  struct uk_thread *child __unused,
				  struct uk_thread *parent __unused)
{
	if (!(cl_args->flags & CLONE_VM)) {
		uk_pr_warn("Cloning thread without CLONE_VM being set: Creating of new address spaces are currently not supported.\n");
		return -ENOTSUP;
	}
	return 0;
}
UK_POSIX_CLONE_HANDLER(CLONE_VM, false, uk_posix_clone_checkvm, 0x0);

/*
 * Ignore historical CLONE_DETACHED flag
 */
static int uk_posix_clone_detached(const struct clone_args *cl_args __unused,
				   size_t cl_args_len __unused,
				   struct uk_thread *child __unused,
				   struct uk_thread *parent __unused)
{
	uk_pr_debug("Ignoring historical CLONE_DETACHED\n");
	return 0;
}
UK_POSIX_CLONE_HANDLER(CLONE_DETACHED, false, uk_posix_clone_detached, 0x0);

// ZZC: CLONE_VFORK
static int uk_posix_clone_vfork(const struct clone_args *cl_args __unused,
				  size_t cl_args_len __unused,
				  struct uk_thread *child __unused,
				  struct uk_thread *parent __unused)
{
	UK_WARN_STUBBED();
	return 0;
}
UK_POSIX_CLONE_HANDLER(CLONE_VFORK, true, uk_posix_clone_vfork, 0x0);	// 这个宏注册的函数似乎会在clone的过程中全部跑一遍
// ZZC-end