# vgpu userspace test, statically linked (the guest has no
# package manager; see docs/adr/0003).

CC ?= gcc
CFLAGS := -O2 -Wall -Wextra -static -I../driver/uapi -I/usr/include/libdrm

vgpu_test: vgpu_test.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f vgpu_test

.PHONY: clean
