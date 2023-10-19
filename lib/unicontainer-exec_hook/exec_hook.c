// #include "exec_hook.h"
#include <uk/print.h>
#include <uk/arch/lcpu.h>
// #include <arch/regmap_linuxabi.h>

// #define rip		rcx
// #define rsyscall	orig_rax
// #define rarg0		rdi
// #define rarg1		rsi
// #define rarg2		rdx
// #define rarg3		r10
// #define rarg4		r8
// #define rarg5		r9

// #define rret0		rax
// #define rret1		rdx

#define NULL (0)

int exec_hook(struct __regs *r){
    uk_pr_warn("[unicontainer]ZZZZZZZZZZZZZZZZZZZZZZZZZC\n");
    uk_pr_warn("[unicontainer]syscall number:%lu, arg0:%lu, arg1:%lu, arg2:%lu\n", r->rax, r->rdi, r->rsi, r->rdx);
    uk_pr_warn("[unicontainer]%c\n", *((char**)r->rdi));
    uk_pr_warn("[unicontainer]argv: ");
    int i = 0;
    while (*((char**)(r->rdi)+i) != NULL)
    {
        uk_pr_warn("%s ", ((char**)(r->rdi)+i));
        i++;
    }
    uk_pr_warn("\n");

    
    return 0;
}