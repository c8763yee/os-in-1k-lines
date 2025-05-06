#include "user.h"

int main(void)
{
	while (1) {
prompt:
		printf("shell> ");
		char cmdline[123];
		for (int i = 0;; i++) {
			char ch = getchar();
			putchar(ch);
			if (i >= sizeof(cmdline) - 1) { // input command
				printf("\nCommand too long\n");
				goto prompt;
			} else if (ch ==
				   '\r') { // complete command(it's \r in qemu terminal)
				printf("\n");
				cmdline[i] = 0;
				break;
			} else {
				cmdline[i] = ch;
			}
		}

		// command processing
		if (strcmp(cmdline, "hello") == 0) {
			printf("Hi!\n");
		} else if (strcmp(cmdline, "exit") == 0) {
			exit();
		} else {
			printf("Unknown command: %s\n", cmdline);
		}
	}
}
