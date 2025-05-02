#include "user.h"

void main(void)
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
			} else if (ch == '\r') { // complete command
				printf("\n");
				cmdline[i] = 0;
				break;
			} else {
				cmdline[i] = ch;
			}
		}

		// command processing
		if (strcmp(cmdline, "hello")) {
			printf("Hi!\n");
		} else {
			printf("Unknown command: %s\n", cmdline);
		}
	}
}
