#include <stdlib.h>
#include <string.h>
#include <uk/print.h>
#include <uk/arch/lcpu.h>
#include <uk/thread.h>
#include <uk/semaphore.h>
#include "exec_hook.h"

/** execve definition:
 *      int execve(const char *filename, char *const argv[], char *const envp[]);
 * original rip for user programs: rcx
 * system call number: orig_rax
 * 64-bit register parameter passing order: rdi, rsi, rdx, r10, r8, r9
 * return value: rax
 * 
 * VMCALL nr: rax
 * VMCALL parameter: rbx, rcx, rdx, rsi 
 */

#define NULL                        ((void *) 0)
#define KVM_HC_CLOCK_PAIRING		9
#define VMCALL_UNICONTAINER_EXEC 	90
#define DATA_SIZE                   600
#define STR_SIZE                    50
#define SYS_exit_group              94
#define TMP_ARGV_SIZE               10


static inline long kvm_hypercall3(unsigned int nr, unsigned long p1,
                  unsigned long p2, unsigned long p3)
{
    long ret;
    asm volatile("vmcall"
             : "=a"(ret)
             : "a"(nr), "b"(p1), "c"(p2), "d"(p3)
             : "memory");
    return ret;
}

static inline long kvm_hypercall4(unsigned int nr, unsigned long p1,
				  unsigned long p2, unsigned long p3,
				  unsigned long p4)
{
	long ret;

	asm volatile("vmcall"
		     : "=a"(ret)
		     : "a"(nr), "b"(p1), "c"(p2), "d"(p3), "S"(p4)
		     : "memory");
	return ret;
}

int exec_hook(struct __regs *r){
    // unsigned int nr = r->rax;
    char *filename = (char*)(r->rdi);
    char **argv = (char**)(r->rsi);
    char **envp = (char**)(r->rdx);
    char *ret_context = NULL;
    unsigned int i = 0;
    long ret;
    struct uk_thread *t;
    char* tmp_argv[TMP_ARGV_SIZE];

    // if(SYS_exit_group == r->rsyscall){
    //     UK_CRASH("[unicontainer]exit_group() Exiting\n");
    // }

    t = uk_thread_current();
    UK_ASSERT(t);
    uk_semaphore_up(t->parent->vfork_sem);

    uk_pr_debug("[unicontainer]ZZZZZZZZZZZZZZZZZZZZZZZZZC\n");
    // uk_pr_debug("[unicontainer]syscall number:%lu, arg0:%lu, arg1:%lu, arg2:%lu\n", r->rax, filename, argv, envp);
    uk_pr_debug("[unicontainer]argv[0]: %s\n", argv[0]);
    uk_pr_debug("[unicontainer]envp[0]: %s\n", envp[0]);
    uk_pr_debug("[unicontainer]argv: ");
    while (*((char**)(argv)+i) != 0)
    {
        uk_pr_debug("%s ", (*((char**)(argv)+i)));
        i++;
    }
    uk_pr_debug("\n");

    // 给 ret_context 分配空间
    ret_context = (char*)malloc(DATA_SIZE);
    if(NULL == ret_context){
        uk_pr_warn("[unicontainer]malloc() fail in exec_hook()\n");
        UK_CRASH("[unicontainer]malloc() fail, exec_hook() Exiting\n");
    }

    // TODO: 如果传入的 argv 是 “sh -c xxx” 格式的，要把 “sh -c” 去掉，只留下后面的内容
    if(strcmp("-c", argv[1]) == 0){
        i = 0;
        while (argv[i+2]) {
            tmp_argv[i] = (char*)malloc(STR_SIZE);  // 给 tmp_argv 分配空间
            if(NULL == tmp_argv[i]){
                uk_pr_warn("[unicontainer]malloc() fail in exec_hook()\n");
                UK_CRASH("[unicontainer]malloc() fail, exec_hook() Exiting\n");
            }
            strcpy(tmp_argv[i], argv[i+2]);     // 把参数往前移两个位置，赋值给 tmp_argv
            i++;
        }
        tmp_argv[i] = NULL;
    }

    ret = kvm_hypercall4(VMCALL_UNICONTAINER_EXEC, (unsigned long)tmp_argv[0], (unsigned long)tmp_argv, (unsigned long)envp, (unsigned long)ret_context);
    uk_pr_debug("[unicontainer]return value from kvm vmcall: %ld\n", ret);
    uk_pr_debug("[unicontainer]return context from kvm vmcall: %s\n", ret_context);
    // strcpy(ret_context, "[unicontainer]exec_hook() debug, if you see this message in the official version, change the code of exec_hook().\n");

    // uk_syscall_r_write(1, ret_context, strlen(ret_context));
    uk_syscall_r_write(4, ret_context, strlen(ret_context));    // 这里把4写死了，但实际上 pipe 的两个文件描述符不一定是3和4
    // uk_syscall_r_write(7, ret_context, strlen(ret_context));

    free(ret_context);

    for(i = 0; i < TMP_ARGV_SIZE; i++){
        if(NULL != tmp_argv[i]){
            free(tmp_argv[i]);
        }
    }

    uk_syscall_r_exit(0);   // 退出当前线程

    UK_CRASH("[unicontainer]exec_hook() Exiting\n");    // 应该不会执行到这里才对

    return 0;
}
