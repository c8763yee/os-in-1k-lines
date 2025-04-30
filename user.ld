ENTRY(start)

SECTIONS {
	. = 0x1000000; /* base address */

	/* .text: Machine code */
	.text : {
		KEEP(*(.text.start));
		*(.text .text.*);
	}

	/* rodata: Read-Only Data */ 
	.rodata : ALIGN(4){
		*(.rodata .rodata.*);
	}

	/*.data: Initialized Data */ 
	.data : ALIGN(4) {
		*(.data .data.*);
	}

	/* bss: Initialized Data that will be filled with zero when startup */ 
	.bss : ALIGN(4) {
		*(.bss .bss.* .sbss .sbss.*);

		. = ALIGN(16);
		. += 64 * 1024; /* 64KB */
		__stack_top = .;
		
		ASSERT(. < 0x1800000, "too large executable");

	}
}
