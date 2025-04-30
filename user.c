#include "user.h"

extern char __stack_top[];

__attribute__((noreturn)) void exit(void)
{
	for (;;)
		;
}

void putchar(char ch)
{
	// TODO
	
}

__attribute__((section(".text.start"))) __attribute__((naked)) void start(void)
{
	// call main and exit via asm
	__asm__ __volatile__(
		"mv sp, %[stack_top]\n" // Set stack pointer
		"call main\n"
		"call exit\n" ::[stack_top] "r"(__stack_top) // stack top
	);
}
