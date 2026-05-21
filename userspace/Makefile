CC = gcc
CFLAGS = -Wall -Wextra -O2

all: fsctl

fsctl: fsctl.c
	$(CC) $(CFLAGS) -o fsctl fsctl.c
	@echo "Userspace utility compiled successfully."

clean:
	rm -f fsctl
	@echo "Userspace cleaned."