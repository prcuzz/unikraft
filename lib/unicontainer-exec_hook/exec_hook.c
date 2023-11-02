#include <stdlib.h>
#include <uk/print.h>
#include <uk/arch/lcpu.h>

// execve definition:
//      int execve(const char *filename, char *const argv[], char *const envp[]);

// original rip for user programs: rcx
// system call number: orig_rax
// 64-bit register parameter passing order: rdi, rsi, rdx, r10, r8, r9
// return value: rax

// VMCALL nr: rax
// VMCALL parameter: rbx, rcx, rdx, rsi

#define NULL (0)
#define KVM_HC_CLOCK_PAIRING		9
#define VMCALL_UNICONTAINER_EXEC 	90

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

int exec_hook(struct __regs *r){
    unsigned int nr = r->rax;
    char *filename = r->rdi;
    char **argv = r->rsi;
    char **envp = r->rdx;

    uk_pr_warn("[unicontainer]ZZZZZZZZZZZZZZZZZZZZZZZZZC\n");
    uk_pr_warn("[unicontainer]syscall number:%lu, arg0:%lu, arg1:%lu, arg2:%lu\n", r->rax, r->rdi, r->rsi, r->rdx);
    uk_pr_warn("[unicontainer]argv[0]: %s\n", argv[0]);
    uk_pr_warn("[unicontainer]envp[0]: %s\n", envp[0]);
    uk_pr_warn("[unicontainer]argv: ");
    unsigned int i = 0;
    while (*((char**)(r->rsi)+i) != 0)
    {
        // uk_pr_warn("%lu ", (*((char**)(r->rsi)+i)));
        uk_pr_warn("%s ", (*((char**)(r->rsi)+i)));
        i++;
    }
    uk_pr_warn("\n");

    long ret;
    // ret = kvm_hypercall2(KVM_HC_CLOCK_PAIRING, 0, 0);
    ret = kvm_hypercall3(VMCALL_UNICONTAINER_EXEC, r->rdi, r->rsi, r->rdx);
    uk_pr_warn("[unicontainer]return value from kvm vmcall: %ld\n", ret);

    UK_CRASH("[unicontainer]Exiting\n");
    return 0;
}
