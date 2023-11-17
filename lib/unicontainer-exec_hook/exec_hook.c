#include <stdlib.h>
#include <uk/print.h>
#include <uk/arch/lcpu.h>
#include "exec_hook.h"

// execve definition:
//      int execve(const char *filename, char *const argv[], char *const envp[]);

// original rip for user programs: rcx
// system call number: orig_rax
// 64-bit register parameter passing order: rdi, rsi, rdx, r10, r8, r9
// return value: rax

// VMCALL nr: rax
// VMCALL parameter: rbx, rcx, rdx, rsi

#define NULL                        ((void *) 0)
#define KVM_HC_CLOCK_PAIRING		9
#define VMCALL_UNICONTAINER_EXEC 	90
#define DATA_SIZE                   600
#define SYS_exit_group              94

static inline long kvm_hypercall2(unsigned int nr, unsigned long p1,
                  unsigned long p2)
{
    long ret;
    asm volatile("vmcall"
             : "=a"(ret)
             : "a"(nr), "b"(p1), "c"(p2)
             : "memory");
    return ret;
}

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

    // if(SYS_exit_group == r->rsyscall){
    //     UK_CRASH("[unicontainer]exit_group() Exiting\n");
    // }

    uk_pr_debug("[unicontainer]ZZZZZZZZZZZZZZZZZZZZZZZZZC\n");
    uk_pr_debug("[unicontainer]syscall number:%lu, arg0:%lu, arg1:%lu, arg2:%lu\n", r->rax, r->rdi, r->rsi, r->rdx);
    uk_pr_debug("[unicontainer]argv[0]: %s\n", argv[0]);
    uk_pr_debug("[unicontainer]envp[0]: %s\n", envp[0]);
    uk_pr_debug("[unicontainer]argv: ");
    while (*((char**)(r->rsi)+i) != 0)
    {
        uk_pr_debug("%s ", (*((char**)(r->rsi)+i)));
        i++;
    }
    uk_pr_debug("\n");

    ret_context = (char*)malloc(DATA_SIZE);
    if(NULL == ret_context){
        uk_pr_warn("[unicontainer]malloc() fail in exec_hook()\n");
        UK_CRASH("[unicontainer]malloc() fail, exec_hook() Exiting\n");
    }

    ret = kvm_hypercall4(VMCALL_UNICONTAINER_EXEC, r->rdi, r->rsi, r->rdx, (unsigned long)ret_context);
    uk_pr_debug("[unicontainer]return value from kvm vmcall: %ld\n", ret);
    uk_pr_debug("[unicontainer]return context from kvm vmcall: %s\n", ret_context);
    write(1, ret_context, strlen(ret_context));
    write(1, "\n", strlen("\n"));

    free(ret_context);

    UK_CRASH("[unicontainer]exec_hook() Exiting\n");
    return 0;
}
