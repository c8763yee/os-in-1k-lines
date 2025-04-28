#include "kernel.h"
#include "common.h"
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint32_t size_t;

extern char __bss[], __bss_end[], __stack_top[], __free_ram[], __free_ram_end[];

struct process procs[PROCS_MAX];

struct sbiret sbi_call(long arg0, long arg1, long arg2, long arg3, long arg4,
		       long arg5, long fid, long eid)
{
	register long a0 __asm__("a0") = arg0;
	register long a1 __asm__("a1") = arg1;
	register long a2 __asm__("a2") = arg2;
	register long a3 __asm__("a3") = arg3;
	register long a4 __asm__("a4") = arg4;
	register long a5 __asm__("a5") = arg5;
	register long a6 __asm__("a6") = fid;
	register long a7 __asm__("a7") = eid;

	__asm__ __volatile__("ecall"
			     : "=r"(a0), "=r"(a1)
			     : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(a4),
			       "r"(a5), "r"(a6), "r"(a7)
			     : "memory");
	return (struct sbiret){ .error = a0, .value = a1 };
}

void putchar(char ch)
{
	sbi_call(ch, 0, 0, 0, 0, 0, 0, 1 /* Console Putchar */);
}
__attribute__((naked)) void switch_context(uint32_t *prev_sp, uint32_t *next_sp)
{
	__asm__ __volatile__(
		"addi sp, sp, -13 * 4\n" // Allocate space for 13 4-byets registers

		// Save callee-saved registers to stack
		"sw ra,  4 * 0(sp)\n"
		"sw s0, 4 * 1(sp)\n"
		"sw s1, 4 * 2(sp)\n"
		"sw s2, 4 * 3(sp)\n"
		"sw s3, 4 * 4(sp)\n"
		"sw s4, 4 * 5(sp)\n"
		"sw s5, 4 * 6(sp)\n"
		"sw s6, 4 * 7(sp)\n"
		"sw s7, 4 * 8(sp)\n"
		"sw s8, 4 * 9(sp)\n"
		"sw s9, 4 * 10(sp)\n"
		"sw s10, 4 * 11(sp)\n"
		"sw s11, 4 * 12(sp)\n"

		// switch stack pointer
		"sw sp, (a0)\n"
		"lw sp, (a1)\n"

		// Restore callee-saved register
		"lw ra,  0  * 4(sp)\n" // Restore callee-saved registers only
		"lw s0,  1  * 4(sp)\n"
		"lw s1,  2  * 4(sp)\n"
		"lw s2,  3  * 4(sp)\n"
		"lw s3,  4  * 4(sp)\n"
		"lw s4,  5  * 4(sp)\n"
		"lw s5,  6  * 4(sp)\n"
		"lw s6,  7  * 4(sp)\n"
		"lw s7,  8  * 4(sp)\n"
		"lw s8,  9  * 4(sp)\n"
		"lw s9,  10 * 4(sp)\n"
		"lw s10, 11 * 4(sp)\n"
		"lw s11, 12 * 4(sp)\n"
		"addi sp, sp, 13 * 4\n" // We've popped 13 4-byte registers from the stack
		"ret\n");
}

struct process *create_process(uint32_t pc)
{
	struct process *proc = NULL;
	int i;

	// find unused process
	for (i = 0; i < PROCS_MAX; i++) {
		if (procs[i].state == PROC_UNUSED) {
			proc = &procs[i];
			break;
		}
	}

	if (!proc)
		PANIC("NONE OF FREE PROCESS SPOT CAN BE USED");

	uint32_t *sp = (uint32_t *)&proc->stack[sizeof(proc->stack)];
	for (int s = 11; s >= 0; s--) {
		*--sp = 0; // set s11 ~ s0 to 0
	}
	*--sp = (uint32_t)pc; // set ra to pc

	// initialize fields
	proc->pid = i + 1;
	proc->state = PROC_RUNNABLE;
	proc->sp = (uint32_t)sp;
	return proc;
}

void delay(void)
{
	for (int i = 0; i < 30000000; i++)
		__asm__ __volatile__("nop");
}

void proc_a_entry(void)
{
	printf("starting process A\n");
	while (1) {
		putchar('A');
		switch_context(&proc_a->sp, &proc_b->sp);
		delay();
	}
}

void proc_b_entry(void)
{
	printf("starting process B\n");
	while (1) {
		putchar('B');
		switch_context(&proc_b->sp, &proc_a->sp);
		delay();
	}
}

paddr_t alloc_pages(uint32_t npage)
{
	static paddr_t next_paddr = (paddr_t)__free_ram;
	paddr_t paddr = next_paddr;
	next_paddr += npage * PAGE_SIZE;

	// check if the next_paddr is within the free RAM range
	if (next_paddr > (paddr_t)__free_ram_end) {
		PANIC("Out of memory");
	}

	memset((void *)paddr, 0,
	       npage * PAGE_SIZE); // Clear the allocated memory
	return paddr;
}

void handle_trap(struct trap_frame *f)
{
	uint32_t cause = READ_CSR(scause);
	uint32_t stval = READ_CSR(stval);
	uint32_t user_pc = READ_CSR(sepc);
	uint32_t sstatus = READ_CSR(sstatus);
	PANIC("Trap occurred: scause = %x, stval = %x, sepc = %x, sstatus = %x\n",
	      cause, stval, user_pc, sstatus);
}

__attribute__((naked)) __attribute__((aligned(4))) void kernel_entry(void)
{
	__asm__ __volatile__(
		"csrw sscratch, sp\n" // use sscratch as temp SP storage

		// save all regesters to stack
		"addi sp, sp, -4 * 31\n"
		"sw ra,  4 * 0(sp)\n"
		"sw gp,  4 * 1(sp)\n"
		"sw tp,  4 * 2(sp)\n"
		"sw t0,  4 * 3(sp)\n"
		"sw t1,  4 * 4(sp)\n"
		"sw t2,  4 * 5(sp)\n"
		"sw t3,  4 * 6(sp)\n"
		"sw t4,  4 * 7(sp)\n"
		"sw t5,  4 * 8(sp)\n"
		"sw t6,  4 * 9(sp)\n"
		"sw a0,  4 * 10(sp)\n"
		"sw a1,  4 * 11(sp)\n"
		"sw a2,  4 * 12(sp)\n"
		"sw a3,  4 * 13(sp)\n"
		"sw a4,  4 * 14(sp)\n"
		"sw a5,  4 * 15(sp)\n"
		"sw a6,  4 * 16(sp)\n"
		"sw a7,  4 * 17(sp)\n"
		"sw s0,  4 * 18(sp)\n"
		"sw s1,  4 * 19(sp)\n"
		"sw s2,  4 * 20(sp)\n"
		"sw s3,  4 * 21(sp)\n"
		"sw s4,  4 * 22(sp)\n"
		"sw s5,  4 * 23(sp)\n"
		"sw s6,  4 * 24(sp)\n"
		"sw s7,  4 * 25(sp)\n"
		"sw s8,  4 * 26(sp)\n"
		"sw s9,  4 * 27(sp)\n"
		"sw s10, 4 * 28(sp)\n"
		"sw s11, 4 * 29(sp)\n"

		// save original SP
		"csrr a0, sscratch\n"
		"sw a0, 4 * 30(sp)\n"

		// calling `handle_trap(sp)` to handle the trap
		"mv a0, sp\n"
		"call handle_trap\n"

		// restore all registers from stack after trap
		"lw ra,  4 * 0(sp)\n"
		"lw gp,  4 * 1(sp)\n"
		"lw tp,  4 * 2(sp)\n"
		"lw t0,  4 * 3(sp)\n"
		"lw t1,  4 * 4(sp)\n"
		"lw t2,  4 * 5(sp)\n"
		"lw t3,  4 * 6(sp)\n"
		"lw t4,  4 * 7(sp)\n"
		"lw t5,  4 * 8(sp)\n"
		"lw t6,  4 * 9(sp)\n"
		"lw a0,  4 * 10(sp)\n"
		"lw a1,  4 * 11(sp)\n"
		"lw a2,  4 * 12(sp)\n"
		"lw a3,  4 * 13(sp)\n"
		"lw a4,  4 * 14(sp)\n"
		"lw a5,  4 * 15(sp)\n"
		"lw a6,  4 * 16(sp)\n"
		"lw a7,  4 * 17(sp)\n"
		"lw s0,  4 * 18(sp)\n"
		"lw s1,  4 * 19(sp)\n"
		"lw s2,  4 * 20(sp)\n"
		"lw s3,  4 * 21(sp)\n"
		"lw s4,  4 * 22(sp)\n"
		"lw s5,  4 * 23(sp)\n"
		"lw s6,  4 * 24(sp)\n"
		"lw s7,  4 * 25(sp)\n"
		"lw s8,  4 * 26(sp)\n"
		"lw s9,  4 * 27(sp)\n"
		"lw s10, 4 * 28(sp)\n"
		"lw s11, 4 * 29(sp)\n"
		"lw sp,  4 * 30(sp)\n"
		"sret\n");
}

struct process *current_proc, *idle_proc;

void yield(void)
{
	// Find runnable process
	struct process *next = idle_proc;
	for (int i = 0; i < PROCS_MAX; i++) {
		struct process *proc =
			&procs[(current_proc->pid + 1) % PROCS_MAX];
		if (proc->state == PROC_RUNNABLE && proc->pid > 0) {
			next = proc;
			break;
		}
	}
	// return if no available process
	if (next == current_proc)
		return;
	// context switch
	struct process *prev = current_proc;
	current_proc = next;
	switch_context(&prev->sp, &next_sp);
}

void kernel_main(void)
{
	memset(__bss, 0, (size_t)(__bss_end - __bss)); // Clear the BSS section
	WRITE_CSR(stvec, (uint32_t)kernel_entry);

	proc_a = create_process((uint32_t)proc_a_entry);
	proc_b = create_process((uint32_t)proc_b_entry);
	proc_a_entry();

	PANIC("DONE");
}

__attribute__((section(".text.boot"))) __attribute__((naked)) void boot(void)
{
	__asm__ __volatile__(
		"mv sp, %[stack_top]\n" // Set the stack pointer
		"j kernel_main\n" // Jump to the kernel main function
		:
		: [stack_top] "r"(
			__stack_top) // Pass the stack top address as %[stack_top]
	);
}
