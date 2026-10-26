# Host build: unit tests and the PC simulation. Target builds happen in STM32CubeIDE.
#   make test     - build and run the host unit tests
#   make sim      - build and run the PC simulation (demo application)
#   make target   - compile-check the target sources with arm-none-eabi-gcc (see port/stm32h5/check.mk)

CC      ?= gcc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror \
           -fsanitize=address,undefined -fno-omit-frame-pointer
MBED_DIR = third_party/mbedtls
MBED_FLAGS = -DMBEDTLS_CONFIG_FILE='"tls_config.h"' -Iplatform/net -I$(MBED_DIR)/include -I$(MBED_DIR)/library
CPPFLAGS = -DPLAT_HOST -Iplatform/include -Iplatform/bsp -Iapp_demo $(MBED_FLAGS)

CORE = platform/core/scan_engine.c platform/core/plat_cmdq.c \
       platform/core/task_monitor.c platform/core/plat_diag.c platform/core/param.c \
       platform/core/param_store.c platform/core/plat_crc32.c platform/core/plat_sys.c platform/core/modbus.c platform/core/plat_sha256.c platform/core/plat_log.c platform/core/plat_alarm.c platform/core/plat_state.c platform/core/plat_power.c platform/core/plat_cal.c platform/core/plat_service.c platform/core/plat_selftest.c platform/core/plat_clock.c platform/core/fw_update.c platform/core/fw_pubkey.c platform/core/dev_pubkey.c platform/core/plat_serial.c platform/core/mb_rtu.c platform/core/iob_net.c platform/core/iobus.c platform/core/w5500.c platform/core/iob_w5500.c \
       platform/net/mbtcp.c platform/net/iob_tcp.c platform/net/http_util.c platform/net/http_api.c platform/net/web_auth.c platform/net/web_page.c platform/net/log_csv.c platform/net/tls.c platform/net/mail.c platform/net/sntp.c platform/net/fw_sig.c platform/ui/eve.c platform/ui/eve_fonts.c platform/ui/ui.c platform/ui/ui_app.c \
       platform/bsp/nvm_ram.c platform/bsp/nor_sim.c platform/bsp/fw_flash_sim.c platform/bsp/iob_node_sim.c platform/bsp/w5500_sim.c platform/bsp/bsp_host.c

BUILD = build

# The application linked into netsim and the target check (request R1). The default is
# the demo. Another application lives in its own directory with an app_vars.mk that sets
# APP_SRC (its .c files) and APP_INC (its -I flags), and defines app_select():
#   make netsim APP=app_vmmux        make target APP=app_vmmux
APP ?= app_demo
ifeq ($(APP),app_demo)
APP_SRC = app_demo/app_demo.c
APP_INC = -Iapp_demo
APP_HDRS = $(wildcard app_demo/*.h)
NETSIM_BIN = $(BUILD)/netsim
else
include $(APP)/app_vars.mk
NETSIM_BIN = $(BUILD)/netsim-$(APP)
endif
export APP APP_SRC APP_INC

# mbedTLS (third party: built once, without the strict warnings and sanitizers)
MBED_SRC = $(wildcard $(MBED_DIR)/library/*.c)
MBED_OBJ = $(patsubst $(MBED_DIR)/library/%.c,$(BUILD)/mbed/%.o,$(MBED_SRC))
MBED_LIB = $(BUILD)/libmbedtls_cems.a
HDRS = $(wildcard platform/include/*.h platform/net/*.h platform/bsp/*.h app_demo/*.h)
$(BUILD)/mbed/%.o: $(MBED_DIR)/library/%.c platform/net/tls_config.h
	@mkdir -p $(BUILD)/mbed
	$(CC) -std=c11 -O2 -g $(MBED_FLAGS) -c $< -o $@
$(MBED_LIB): $(MBED_OBJ)
	ar rcs $@ $^

.PHONY: all test sim target rtos netsim clean
all: test sim

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/tests: $(CORE) tests/test_main.c tests/test_scan.c tests/test_misc.c tests/test_param.c tests/test_store.c tests/test_blob.c tests/test_modbus.c tests/test_http.c platform/net/http_server.c tests/test_log.c tests/test_fw.c tests/test_ui.c tests/test_tls.c tests/test_alarm.c tests/test_clock.c tests/test_serial.c tests/test_ui_app.c tests/test_iobus.c tests/test_iobtcp.c tests/test_w5500.c tests/test_state.c tests/test_power.c tests/test_cal.c tests/test_service.c tests/test_selftest.c host/eve_sim/eve_sim.c host/eve_sim/zinflate.c $(MBED_LIB) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DPLAT_SEQ_TEST -Itests -Ihost/eve_sim $(filter-out %.h,$^) -o $@ -lm

$(BUILD)/sim: $(CORE) app_demo/app_demo.c host/sim_main.c $(MBED_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ -lm

test: $(BUILD)/tests $(BUILD)/tests32
	./$(BUILD)/tests
	./$(BUILD)/tests32 | tail -1

# the same tests with the 32 KB FRAM memory map (the development breakout)
$(BUILD)/tests32: $(CORE) tests/test_main.c tests/test_scan.c tests/test_misc.c tests/test_param.c tests/test_store.c tests/test_blob.c tests/test_modbus.c tests/test_http.c platform/net/http_server.c tests/test_log.c tests/test_fw.c tests/test_ui.c tests/test_tls.c tests/test_alarm.c tests/test_clock.c tests/test_serial.c tests/test_ui_app.c tests/test_iobus.c tests/test_iobtcp.c tests/test_w5500.c tests/test_state.c tests/test_power.c tests/test_cal.c tests/test_service.c tests/test_selftest.c host/eve_sim/eve_sim.c host/eve_sim/zinflate.c $(MBED_LIB) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DPLAT_SEQ_TEST -DPLAT_FRAM_SIZE=32768u -Itests -Ihost/eve_sim $(filter-out %.h,$^) -o $@ -lm

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
           platform/core/plat_sys.c platform/core/modbus.c platform/core/plat_sha256.c platform/core/plat_log.c platform/core/plat_alarm.c platform/core/plat_state.c platform/core/plat_power.c platform/core/plat_cal.c platform/core/plat_service.c platform/core/plat_selftest.c platform/core/plat_clock.c platform/core/fw_update.c platform/core/fw_pubkey.c platform/core/dev_pubkey.c platform/core/plat_serial.c platform/core/mb_rtu.c platform/core/iob_net.c platform/core/iobus.c platform/core/w5500.c platform/core/iob_w5500.c platform/net/iob_tcp.c platform/net/web_auth.c platform/net/tls.c platform/net/mail.c platform/net/sntp.c platform/net/fw_sig.c platform/ui/ui_app.c \
           platform/bsp/nvm_ram.c platform/bsp/nor_sim.c platform/bsp/fw_flash_sim.c platform/os/plat_tasks_freertos.c platform/os/plat_store_task.c platform/os/plat_iobus_task.c app_demo/app_demo.c \
           host/rtos_sim/bsp_posix.c host/rtos_sim/rtos_main.c \
           $(FR)/tasks.c $(FR)/list.c $(FR)/queue.c $(FR)/timers.c $(FR)/portable/MemMang/heap_3.c \
           $(FR)/portable/ThirdParty/GCC/Posix/port.c $(FR)/portable/ThirdParty/GCC/Posix/utils/wait_for_event.c
$(BUILD)/rtos_sim: $(RTOS_SRC) $(MBED_LIB) | $(BUILD)
	$(CC) -std=gnu11 -O1 -g -Wall -DPLAT_POSIX_SOCKETS -Iplatform/include -Iplatform/bsp -Iapp_demo -Ihost/rtos_sim $(MBED_FLAGS) \
	  -I$(FR)/include -I$(FR)/portable/ThirdParty/GCC/Posix -I$(FR)/portable/ThirdParty/GCC/Posix/utils \
	  $^ -o $@ -lpthread -lm
rtos: $(BUILD)/rtos_sim
	./$(BUILD)/rtos_sim normal
	./$(BUILD)/rtos_sim stuck; test $$? -eq 42 && echo "stuck-task reset: OK"

# PC simulation with real sockets and threads (Modbus TCP on 1502, web on 8080)
BUILD_ID  := $(shell git rev-parse --short=10 HEAD 2>/dev/null || echo dev)
NETSIM_SRC = $(filter-out platform/bsp/bsp_host.c,$(CORE)) $(APP_SRC) platform/net/http_server.c host/netsim/netsim_main.c $(MBED_LIB)
$(NETSIM_BIN): $(NETSIM_SRC) $(HDRS) $(APP_HDRS) | $(BUILD)
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -DPLAT_HOST -DPLAT_HOST_THREADS -DPLAT_BUILD_ID='"$(BUILD_ID)"' -Iplatform/include -Iplatform/bsp $(APP_INC) $(MBED_FLAGS) $(filter-out %.h,$^) -o $@ -lm -lpthread
netsim: $(NETSIM_BIN)
	./$(NETSIM_BIN)
