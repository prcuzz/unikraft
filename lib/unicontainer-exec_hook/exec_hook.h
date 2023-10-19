#define rip		rcx
#define rsyscall	orig_rax
#define rarg0		rdi
#define rarg1		rsi
#define rarg2		rdx
#define rarg3		r10
#define rarg4		r8
#define rarg5		r9

#define rret0		rax
#define rret1		rdx

int exec_hook(struct __regs *r);