#include "kernel.h"
#include "common.h"
typedef unsigned char uint8_t;
typedef unsigned int uint32_t;
typedef uint32_t size_t;

extern char __bss[], __bss_end[], __stack_top[], __free_ram[], __free_ram_end[],
	__kernel_base[];
extern char _binary_shell_bin_start[];
extern char _binary_shell_bin_size[];

struct process procs[PROCS_MAX];

struct process *current_proc, *idle_proc;
struct process *proc_a, *proc_b;

struct virtio_virtq *blk_request_vq;
struct virtio_blk_req *blk_req;
paddr_t bld_req_paddr;
unsigned blk_capacity;

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

int getchar(void)
{
	struct sbiret ret = sbi_call(0, 0, 0, 0, 0, 0, 0, 2);
	return ret.error;
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

void yield(void)
{
	// Find runnable process
	struct process *next = idle_proc;
	for (int i = 0; i < PROCS_MAX; i++) {
		struct process *proc =
			&procs[(current_proc->pid + i) % PROCS_MAX];
		if (proc->state == PROC_RUNNABLE && proc->pid > 0) {
			next = proc;
			break;
		}
	}

	// return if no available process(next is same as current process)
	if (next == current_proc)
		return;

	__asm__ __volatile__(
		// use sfence.vma to
		// 	1. ensure change to page table are properly completed
		// 	2. clear cache of page table entries (TLB)

		"sfence.vma\n"
		"csrw satp, %[satp]\n"
		"sfence.vma\n"
		"csrw sscratch, %[sscratch]\n"
		:
		: [satp] "r"(SATP_SV32 |
			     ((uint32_t)next->page_table / PAGE_SIZE)),
		  [sscratch] "r"((uint32_t)&next->stack[sizeof(next->stack)]));

	// context switch
	struct process *prev = current_proc;
	current_proc = next;
	switch_context(&prev->sp, &next->sp);
}

void proc_a_entry(void)
{
	printf("starting process A\n");
	while (1) {
		putchar('A');
		yield();
	}
}

void proc_b_entry(void)
{
	printf("starting process B\n");
	while (1) {
		putchar('B');
		yield();
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

/// virtual address
/// offset: first 12 bits
/// vpn0: 12 ~ 21 bits
/// vpn1: 22 ~ 31 bits
void map_page(uint32_t *table1, uint32_t vaddr, paddr_t paddr, uint32_t flags)
{
	if (!is_aligned(vaddr, PAGE_SIZE))
		PANIC("unaligned vaddr %x", vaddr);

	if (!is_aligned(paddr, PAGE_SIZE))
		PANIC("unaligned paddr %x", paddr);

	uint32_t vpn1 = (vaddr >> 22) & 0x3ff;
	if ((table1[vpn1] & PAGE_V) == 0) {
		// Create the non-existent 2nd level page table
		uint32_t pt_paddr = alloc_pages(1);
		table1[vpn1] = ((pt_paddr / PAGE_SIZE) << 10) | PAGE_V;
	}

	// Set the 2nd level pages table entry to mapping paddr
	uint32_t vpn0 = (vaddr >> 12) & 0x3ff;
	uint32_t *table0 = (uint32_t *)((table1[vpn1] >> 10) * PAGE_SIZE);
	table0[vpn0] = ((paddr / PAGE_SIZE) << 10) | flags | PAGE_V;
}

// ↓ __attribute__((naked)) is very important!
__attribute__((naked)) void user_entry(void)
{
	__asm__ __volatile__(
		"csrw sepc, %[sepc]        \n"
		"csrw sstatus, %[sstatus]  \n"
		"sret                      \n"
		:
		: [sepc] "r"(USER_BASE), [sstatus] "r"(SSTATUS_SPIE));
}

struct process *create_process(const void *image, size_t image_size)
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
	*--sp = (uint32_t)user_entry; // set ra to pc

	// kernel pages
	uint32_t *page_table = (uint32_t *)alloc_pages(1);
	for (paddr_t paddr = (paddr_t)__kernel_base;
	     paddr <= (paddr_t)__free_ram_end; paddr += PAGE_SIZE)
		map_page(page_table, paddr, paddr, PAGE_R | PAGE_W | PAGE_X);

	// Map virtio_blk MMIO region
	map_page(page_table, VIRTIO_BLK_PADDR, VIRTIO_BLK_PADDR,
		 PAGE_R | PAGE_W);

	// Map user pages
	for (uint32_t off = 0; off < image_size; off += PAGE_SIZE) {
		paddr_t page = alloc_pages(1);

		// handle alignment
		size_t remaining = image_size - off;
		size_t copy_size = remaining > PAGE_SIZE ? PAGE_SIZE :
							   remaining;

		memcpy((void *)page, image + off, copy_size);

		map_page(page_table, USER_BASE + off, page,
			 PAGE_U | PAGE_R | PAGE_W | PAGE_X);
	}

	// initialize fields
	proc->pid = i + 1;
	proc->state = PROC_RUNNABLE;
	proc->page_table = page_table;
	proc->sp = (uint32_t)sp;
	return proc;
}

void handle_syscall(struct trap_frame *f)
{
	switch (f->a3) {
	case SYS_PUTCHAR:
		putchar(f->a0);
		break;
	case SYS_GETCHAR:
		while (1) {
			long ch = getchar();
			if (ch >= 0) {
				f->a0 = ch;
				break;
			}
			yield();
		}
		break;
	case SYS_EXIT:
		printf("Process %d exit\n", current_proc->pid);
		current_proc->state = PROC_UNUSED;
		yield();
		PANIC("UNREACHED");
	default:
		PANIC("Unknown syscall %d", f->a3);
	}
}
void handle_trap(struct trap_frame *f)
{
	uint32_t scause = READ_CSR(scause);
	uint32_t stval = READ_CSR(stval);
	uint32_t user_pc = READ_CSR(sepc);
	uint32_t sstatus = READ_CSR(sstatus);
	if (scause == SCAUSE_ECALL) {
		handle_syscall(f);
		user_pc += 4;
	} else
		PANIC("Unexpected trap occurred: scause = %x, stval = %x, sepc = %x, sstatus = x\n",
		      scause, stval, user_pc, sstatus);
	WRITE_CSR(sepc, user_pc);
}

__attribute__((naked)) __attribute__((aligned(4))) void kernel_entry(void)
{
	__asm__ __volatile__(
		// "csrw sscratch, sp\n" // use sscratch as temp SP storage
		"csrrw sp, sscratch, sp\n"

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

		// reset the kernel stack
		"addi a0, sp, 4 * 31\n"
		"csrw sscratch, a0\n"

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
// virtio register util
uint32_t virtio_reg_read32(unsigned offset)
{
	return *((volatile uint32_t *)(VIRTIO_BLK_PADDR + offset));
}

uint64_t virtio_reg_read64(unsigned offset)
{
	return *((volatile uint64_t *)(VIRTIO_BLK_PADDR + offset));
}

void virtio_reg_write32(unsigned offset, uint32_t value)
{
	*((volatile uint32_t *)(VIRTIO_BLK_PADDR + offset)) = value;
}

void virtio_reg_fetch_and_or32(unsigned offset, uint32_t value)
{
	virtio_reg_write32(offset, virtio_reg_read32(offset) | value);
}

struct virtio_virtq *virtq_init(unsigned index)
{
	paddr_t virtq_paddr = alloc_pages(
		align_up(sizeof(struct virtio_virtq), PAGE_SIZE) / PAGE_SIZE);
	struct virtio_virtq *vq = (struct virtio_virtq *)virtq_paddr;
	vq->queue_index = index;
	vq->used_index = (volatile uint16_t *)&(vq->used.index);

	// 1. Select the queue writing it's index into QueueSel
	virtio_reg_write32(VIRTIO_REG_QUEUE_SEL, index);

	// 2. check if the queue is not already in use
	// 		(NOT Implemented for now)

	// 3. Read Maximum queue size from  QueueNumMax
	// 		(NOT Implemented for now)

	// 4. ALlocate and zero the queue pages in contiguous virtual memory
	// 		(NOT Implemented for now)

	// 5. Notify the device about the queue size(writing to QueueNum)
	virtio_reg_write32(VIRTIO_REG_QUEUE_NUM, VIRTQ_ENTRY_NUM);

	// 6. Nofify the device about the used alignment(writing to QueueAlign)
	virtio_reg_write32(VIRTIO_REG_QUEUE_ALIGN, 0);

	// 7. Write physical addres of first page of the queue into QueuePFN
	virtio_reg_write32(VIRTIO_REG_QUEUE_PFN, virtq_paddr);

	return vq;
}

void virtio_blk_init(void)
{
	if (virtio_reg_read32(VIRTIO_REG_MAGIC) != VIRTIO_MAGIC_VALUE)
		PANIC("VIRTIO MAGIC VALUE ERROR");

	if (virtio_reg_read32(VIRTIO_REG_VERSION) != 1)
		PANIC("Invalid VIRTIO VERSION");
	if (virtio_reg_read32(VIRTIO_REG_DEVICE_ID) != VIRTIO_DEVICE_BLK)
		PANIC("Invalid VIRTIO DEVICE ID");

	// 1. Reset Device
	virtio_reg_write32(VIRTIO_REG_DEVICE_STATUS, 0);

	// 2. Set ACK bit
	virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS, VIRTIO_STATUS_ACK);

	// 3. Set DRIVER status bit
	virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS,
				  VIRTIO_STATUS_DRIVER);

	// 4. Read device feature bits, and write subset featuer bits
	// 		(NOT Implemented for now)

	// 5. Set FEATURES_OK bit
	virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS,
				  VIRTIO_STATUS_FEAT_OK);

	// 6. Re-read device status to ensure FEATURES_OK bit is set
	if ((virtio_reg_read32(VIRTIO_REG_DEVICE_STATUS) &
	     VIRTIO_STATUS_FEAT_OK) != VIRTIO_STATUS_FEAT_OK)
		PANIC("FEATURES_OK bit not set");

	// 7. perform device-specific setup
	blk_request_vq = virtq_init(0);

	// 8. Set DRIVER_OK bit
	virtio_reg_fetch_and_or32(VIRTIO_REG_DEVICE_STATUS,
				  VIRTIO_STATUS_DRIVER_OK);

	// Get disk capacity
	// TODO: Find out the reason why I need to add 0 to the device config
	blk_capacity =
		virtio_reg_read64(VIRTIO_REG_DEVICE_CONFIG + 0) * SECTOR_SIZE;

	printf("blk_capacity = %d\n", blk_capacity);

	// ALlocation region to store requests
	bld_req_paddr =
		alloc_pages(align_up(sizeof(*blk_req), PAGE_SIZE) / PAGE_SIZE);
	blk_req = (struct virtio_blk_req *)bld_req_paddr;
}
// Notifies the device that a new request is available
// desc_index: index of the descriptor in the virtqueue
void virtq_kick(struct virtio_virtq *vq, int desc_index)
{
	vq->avail.ring[vp->avail.index % VIRTQ_ENTRY_NUM] = desc_index;
	vq->avail.index++;

	__sync_synchronize(); // Ensure the write to the avail ring is visible
}

//Return whether there are requests being processed by the device
bool virtq_is_busy(struct virtio_virtq *vq)
{
	return vq->last_used_index != *vq->used_index;
}

// Read/write from /to virtio-blk devices
void read_write_disk(void *buf, unsigned sector, int is_write)
{
	if (sector >= blk_capacity / SECTOR_SIZE) {
		printf("virtio: tried to read/write sector=%d, but capacity is %d\n",
		       sector, blk_capacity / SECTOR_SIZE);

		return;
	}

	blk_req->sector = sector;
}
// kernel main function
void kernel_main(void)
{
	memset(__bss, 0, (size_t)(__bss_end - __bss)); // Clear the BSS section
	WRITE_CSR(stvec, (uint32_t)kernel_entry);

	virtio_blk_init();
	idle_proc = create_process(NULL, 0);
	idle_proc->pid = 0;
	current_proc = idle_proc;

	create_process(_binary_shell_bin_start, (size_t)_binary_shell_bin_size);

	yield();
	PANIC("Switched into idle process");
}

__attribute__((section(".text.boot"))) __attribute__((naked)) void boot(void)
{
	__asm__ __volatile__(
		"mv sp, [stack_top]\n" // Set the stack pointer
		"j kernel_main\n" // Jump to the kernel main function
		:
		: [stack_top] "r"(
			__stack_top) // Pass the stack top address as %[stack_top]
	);
}
