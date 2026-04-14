# ============================================================
# Makefile — Multi-Container Runtime
# ============================================================

CC        = gcc
CFLAGS    = -Wall -Wextra -g -O2 -D_GNU_SOURCE
LDFLAGS   = -lpthread

KDIR     ?= /lib/modules/$(shell uname -r)/build
PWD_DIR  := $(shell pwd)

.PHONY: all clean ci module userspace workloads boilerplate/ci

# Default: build everything
all: userspace module workloads

# ---- User-space runtime ------------------------------------
userspace: engine

engine: engine.c monitor_ioctl.h
	$(CC) $(CFLAGS) -o $@ engine.c $(LDFLAGS)

# ---- Workload binaries -------------------------------------
workloads: workload_cpu workload_io workload_mem

workload_cpu: workload_cpu.c
	$(CC) $(CFLAGS) -o $@ workload_cpu.c

workload_io: workload_io.c
	$(CC) $(CFLAGS) -o $@ workload_io.c

workload_mem: workload_mem.c
	$(CC) $(CFLAGS) -o $@ workload_mem.c

# ---- Kernel module -----------------------------------------
module: Kbuild
	$(MAKE) -C $(KDIR) M=$(PWD_DIR) modules

Kbuild:
	@echo "obj-m := monitor.o" > Kbuild

# ---- CI-safe target (GitHub Actions / no kernel headers) ---
# Builds only user-space; no sudo, module loading, or rootfs required.
ci: engine workloads
	@echo "CI build OK (user-space only)"

# Required by project spec: make -C boilerplate ci
boilerplate/ci: ci

# ---- Clean -------------------------------------------------
clean:
	rm -f engine workload_cpu workload_io workload_mem
	$(MAKE) -C $(KDIR) M=$(PWD_DIR) clean 2>/dev/null || true
	rm -f Kbuild Module.symvers modules.order
	rm -f *.o *.ko *.mod.c *.mod
	rm -f .*.cmd


