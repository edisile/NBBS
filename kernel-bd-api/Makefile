-include system.conf
obj-m += bd_api.o

ifdef SYS_TABLE
FLAGS := -DSYSCALL_TABLE=$(SYS_TABLE)
FLAGS += -DNI_SYSCALL=$(NI_SYSCALL)
FLAGS += -D'TO_BE_REPLACED_MALLOC(x)=__get_free_pages(GFP_KERNEL, x)' -D'TO_BE_REPLACED_FREE(x)=free_pages(x)' -DKERNEL_BD=1
endif
MAJOR := $(shell uname -r | cut -c1)

all:
	make KCPPFLAGS="$(FLAGS)" -C /lib/modules/$(shell uname -r)/build M=$(PWD) modules 

clean:
	make -C /lib/modules/$(shell uname -r)/build M=$(PWD) clean
	rm system.conf

init:
	$(eval SYS_TABLE=$(shell sudo cat /proc/kallsyms  | grep "sys_call_table" | head -n1 | cut -f1 -d' '))
ifeq ($(MAJOR), 5)
	$(eval NI_SYSCALL=$(shell sudo cat /proc/kallsyms  | grep -w "__x64_sys_ni_syscall" | cut -f1 -d' '))
else
	$(eval NI_SYSCALL=$(shell sudo cat /proc/kallsyms  | grep -w "sys_ni_syscall" | cut -f1 -d' '))
endif
	@echo SYS_TABLE=$(SYS_TABLE)
	@echo NI_SYSCALL=$(NI_SYSCALL)
	$(shell echo SYS_TABLE=0x$(SYS_TABLE) > system.conf)
	$(shell echo NI_SYSCALL=0x$(NI_SYSCALL) >> system.conf)
