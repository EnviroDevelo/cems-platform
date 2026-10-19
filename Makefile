# Host build: unit tests and the PC simulation. Target builds happen in STM32CubeIDE.
#   make test     - build and run the host unit tests
#   make sim      - build and run the PC simulation (demo application)
#   make target   - compile-check the target sources with arm-none-eabi-gcc (see port/stm32h5/check.mk)

CC      ?= gcc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror \
           -fsanitize=address,undefined -fno-omit-frame-pointer
CPPFLAGS = -DPLAT_HOST -Iplatform/include -Iplatform/bsp -Iapp_demo

CORE = platform/core/scan_engine.c platform/core/plat_cmdq.c \
       platform/core/task_monitor.c platform/core/plat_diag.c platform/core/param.c \
       platform/core/param_store.c platform/core/plat_crc32.c platform/core/plat_sys.c platform/core/modbus.c platform/net/mbtcp.c \
       platform/bsp/nvm_ram.c platform/bsp/bsp_host.c

BUILD = build

.PHONY: all test sim target rtos clean
all: test sim

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tests: $(CORE) tests/test_main.c tests/test_scan.c tests/test_misc.c tests/test_param.c tests/test_store.c tests/test_blob.c tests/test_modbus.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DPLAT_SEQ_TEST -Itests $^ -o $@ -lm

$(BUILD)/sim: $(CORE) app_demo/app_demo.c host/sim_main.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ -lm

test: $(BUILD)/tests $(BUILD)/tests128
	./$(BUILD)/tests
	./$(BUILD)/tests128 | tail -1

# the same tests with the 128 KB FRAM memory map
$(BUILD)/tests128: $(CORE) tests/test_main.c tests/test_scan.c tests/test_misc.c tests/test_param.c tests/test_store.c tests/test_blob.c tests/test_modbus.c | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DPLAT_SEQ_TEST -DPLAT_FRAM_SIZE=131072u -Itests $^ -o $@ -lm

sim: $(BUILD)/sim
	./$(BUILD)/sim

target:
	$(MAKE) -f port/stm32h5/check.mk

clean:
	rm -rf $(BUILD)

# FreeRTOS POSIX run of the real task layer (needs ../ext/FreeRTOS-Kernel)
EXT ?= $(abspath ../ext)
FR   = $(EXT)/FreeRTOS-Kernel
RTOS_SRC = platform/core/scan_engine.c platform/core/plat_cmdq.c platform/core/task_monitor.c \
           platform/core/plat_diag.c platform/core/param.c platform/core/param_store.c platform/core/plat_crc32.c \
           platform/core/plat_sys.c platform/bsp/nvm_ram.c \
           platform/os/plat_tasks_freertos.c platform/os/plat_store_task.c app_demo/app_demo.c \
           host/rtos_sim/bsp_posix.c host/rtos_sim/rtos_main.c \
           $(FR)/tasks.c $(FR)/list.c $(FR)/queue.c $(FR)/timers.c $(FR)/portable/MemMang/heap_3.c \
           $(FR)/portable/ThirdParty/GCC/Posix/port.c $(FR)/portable/ThirdParty/GCC/Posix/utils/wait_for_event.c
$(BUILD)/rtos_sim: $(RTOS_SRC) | $(BUILD)
	$(CC) -std=gnu11 -O1 -g -Wall -Iplatform/include -Iplatform/bsp -Iapp_demo -Ihost/rtos_sim \
	  -I$(FR)/include -I$(FR)/portable/ThirdParty/GCC/Posix -I$(FR)/portable/ThirdParty/GCC/Posix/utils \
	  $^ -o $@ -lpthread
rtos: $(BUILD)/rtos_sim
	./$(BUILD)/rtos_sim normal
	./$(BUILD)/rtos_sim stuck; test $$? -eq 42 && echo "stuck-task reset: OK"
