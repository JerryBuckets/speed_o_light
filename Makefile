# Module name
MODULE_NAME := project

# Rust program name
RUST_PROGRAM := main

# Kernel build directory
KDIR := /lib/modules/$(shell uname -r)/build

# Build directory for kernel module and Rust program
BUILD_DIR := $(PWD)/build

obj-m := $(MODULE_NAME).o

.PHONY: all rust kernel clean load unload reload

all: rust kernel
	@rm -f *.o *.mod *.mod.o .*.cmd

rust:
	@echo "Compiling Rust program..."
	@mkdir -p $(BUILD_DIR)
	rustc $(RUST_PROGRAM).rs -o $(BUILD_DIR)/$(RUST_PROGRAM)

kernel:
	@echo "Building kernel module..."
	@mkdir -p $(BUILD_DIR)
	$(MAKE) -C $(KDIR) M=$(PWD) modules
	@mv $(MODULE_NAME).ko $(BUILD_DIR)
	@mv $(MODULE_NAME).mod.c $(MODULE_NAME).mod.o $(MODULE_NAME).o $(BUILD_DIR)
	@mv Module.symvers modules.order $(BUILD_DIR) 2>/dev/null || true

load: kernel unload
	sudo insmod $(BUILD_DIR)/$(MODULE_NAME).ko

unload:
	sudo rmmod $(MODULE_NAME) || true

reload: unload load

clean:
	@echo "Cleaning up..."
	@$(MAKE) -C $(KDIR) M=$(PWD) clean
	@rm -rf $(BUILD_DIR) *.o *.ko *.mod.c *.mod.o Module.symvers modules.order .*.cmd

