# vgpu userspace: the test binary and the examples, all statically
# linked (the guest has no package manager; see docs/adr/0003).

CC ?= gcc
CFLAGS := -O2 -Wall -Wextra -static -I../driver/uapi -I/usr/include/libdrm

all: vgpu_test examples/saxpy

vgpu_test: vgpu_test.c
	$(CC) $(CFLAGS) -o $@ $<

examples/saxpy: examples/saxpy.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f vgpu_test examples/saxpy

.PHONY: all clean
