# Host build: unit tests and the PC simulation. Target builds happen in STM32CubeIDE.
#   make test     - build and run the host unit tests
#   make sim      - build and run the PC simulation (demo application)
#   make target   - compile-check the target sources with arm-none-eabi-gcc (see port/stm32h5/check.mk)
#   make boot     - build the first-stage bootloader for the STM32H573 (see boot/boot.mk)
#   make node-test   - the I/O node core's own tests (node/, tests/node: the former io-node repository)
#   make master-real - the host tests with the real node core (node/node.c) in place of iob_node_sim.c

CC      ?= gcc
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror \
           -fsanitize=address,undefined -fno-omit-frame-pointer
MBED_DIR = third_party/mbedtls
MBED_FLAGS = -DMBEDTLS_CONFIG_FILE='"tls_config.h"' -Iplatform/net -I$(MBED_DIR)/include -I$(MBED_DIR)/library
CPPFLAGS = -DPLAT_HOST -Iplatform/include -Iplatform/bsp -Inode -Iapp_demo $(MBED_FLAGS)

CORE = platform/core/scan_engine.c platform/core/plat_cmdq.c \
       platform/core/task_monitor.c platform/core/plat_diag.c platform/core/param.c \
       platform/core/param_store.c platform/core/plat_crc32.c platform/core/plat_sys.c platform/core/modbus.c platform/core/plat_sha256.c platform/core/plat_log.c platform/core/plat_alarm.c platform/core/plat_state.c platform/core/plat_power.c platform/core/plat_cal.c platform/core/plat_service.c platform/core/plat_selftest.c platform/core/plat_boardtest.c platform/core/plat_contract.c platform/core/plat_clock.c platform/core/fw_update.c platform/core/boot_core.c platform/core/fw_pubkey.c platform/core/dev_pubkey.c platform/core/plat_serial.c platform/core/mb_rtu.c platform/core/iob_net.c platform/core/iobus.c platform/core/w5500.c platform/core/iob_w5500.c platform/core/plat_role.c \
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

.PHONY: all test sim target boot rtos netsim clean node-test master-real nodesim
all: test sim

$(BUILD):
	mkdir -p $(BUILD)

TEST_SRC = tests/test_main.c tests/test_scan.c tests/test_misc.c tests/test_param.c tests/test_store.c tests/test_blob.c tests/test_modbus.c tests/test_http.c platform/net/http_server.c tests/test_log.c tests/test_fw.c tests/test_ui.c tests/test_tls.c tests/test_alarm.c tests/test_clock.c tests/test_serial.c tests/test_ui_app.c tests/test_iobus.c tests/test_iobtcp.c tests/test_w5500.c tests/test_state.c tests/test_power.c tests/test_cal.c tests/test_service.c tests/test_selftest.c tests/test_contract.c tests/test_boot.c host/eve_sim/eve_sim.c host/eve_sim/zinflate.c
$(BUILD)/tests: $(CORE) $(TEST_SRC) $(MBED_LIB) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DPLAT_SEQ_TEST -Itests -Ihost/eve_sim $(filter-out %.h,$^) -o $@ -lm

$(BUILD)/sim: $(CORE) app_demo/app_demo.c host/sim_main.c $(MBED_LIB) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ -o $@ -lm

test: $(BUILD)/tests $(BUILD)/tests32 fault-codes-check snapshots-check node-test master-real
	./$(BUILD)/tests
	./$(BUILD)/tests32 | tail -1
	python3 tools/cip_coverage.py
	python3 tests/integration/ui_conventions_test.py

# The published fault and alarm code list (CIP-11.3), written from the firmware's tables.
# make fault-codes: docs/FAULT_CODES.md (the platform's), and with APP=<dir> the product's own
# list in <dir>/FAULT_CODES.md. make test fails if docs/FAULT_CODES.md is out of date.
FAULT_BIN = $(BUILD)/fault_codes-$(APP)
$(FAULT_BIN): $(CORE) $(APP_SRC) tools/fault_codes.c $(MBED_LIB) $(HDRS) $(APP_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -DPLAT_HOST -Iplatform/include -Iplatform/bsp $(APP_INC) $(MBED_FLAGS) $(filter-out %.h,$^) -o $@ -lm
.PHONY: fault-codes fault-codes-check
fault-codes: $(FAULT_BIN)
	./$(FAULT_BIN) --platform > docs/FAULT_CODES.md
ifneq ($(APP),app_demo)
	./$(FAULT_BIN) > $(APP)/FAULT_CODES.md || true
endif
# Interface snapshots (spec 9.3, CIP-14.4/14.5): the Modbus register map and the web API of a
# build, generated and kept in git; make test fails if the Modbus map no longer matches, and
# make conformance checks the API (needs netsim). A difference that is meant needs a new map
# version or core version; then run make snapshots to accept it.
IFACE_DIR = $(if $(filter app_demo,$(APP)),docs/interface/app_demo,$(APP)/interface)
MBMAP_BIN = $(BUILD)/modbus_map-$(APP)
$(MBMAP_BIN): $(CORE) $(APP_SRC) tools/modbus_map.c $(MBED_LIB) $(HDRS) $(APP_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -DPLAT_HOST -Iplatform/include -Iplatform/bsp $(APP_INC) $(MBED_FLAGS) $(filter-out %.h,$^) -o $@ -lm
.PHONY: modbus-map snapshots snapshots-check
modbus-map: $(MBMAP_BIN)
	@mkdir -p $(IFACE_DIR)
	./$(MBMAP_BIN) > $(IFACE_DIR)/MODBUS_MAP.md
snapshots: modbus-map $(NETSIM_BIN)
	python3 tools/api_snapshot.py $(NETSIM_BIN) $(IFACE_DIR)/API.txt
snapshots-check: $(MBMAP_BIN)
	@./$(MBMAP_BIN) > $(BUILD)/MODBUS_MAP-$(APP).md
	@diff -u $(IFACE_DIR)/MODBUS_MAP.md $(BUILD)/MODBUS_MAP-$(APP).md || \
	  { echo "the Modbus map changed (above): if meant, bump the map version (or the core version for the platform block) and run make snapshots"; exit 1; }
	@echo "$(IFACE_DIR)/MODBUS_MAP.md matches the firmware"

# make conformance APP=<dir>: is this product still on the common platform (spec 9.3)? Its start-up
# contract (netsim refuses to start otherwise), its profile filled in, every alarm with a help
# text, its interface snapshots current, and the shared CIP-26 tests run against its build.
.PHONY: conformance
conformance: $(NETSIM_BIN) $(FAULT_BIN) snapshots-check
	@test -f $(APP)/PROFILE.md || { echo "$(APP)/PROFILE.md is missing: start from docs/PROFILE_TEMPLATE.md"; exit 1; }
	@! grep -n '<[a-zA-Z][^>]*>' $(APP)/PROFILE.md || { echo "$(APP)/PROFILE.md still has placeholders (above)"; exit 1; }
	@./$(FAULT_BIN) > $(BUILD)/FAULT_CODES-$(APP).md || { echo "every product alarm needs a .help (above)"; exit 1; }
	python3 tools/api_snapshot.py $(NETSIM_BIN) $(IFACE_DIR)/API.txt --check
	python3 tests/integration/conformance_test.py $(NETSIM_BIN)
	@echo "CONFORMANCE $(APP): PASS"

fault-codes-check: $(FAULT_BIN)
	@./$(FAULT_BIN) --platform > $(BUILD)/FAULT_CODES.md
	@diff -u docs/FAULT_CODES.md $(BUILD)/FAULT_CODES.md > /dev/null || \
	  { echo "docs/FAULT_CODES.md is out of date: run make fault-codes"; exit 1; }
	@echo "docs/FAULT_CODES.md is current"

# the same tests with the 32 KB FRAM memory map (the development breakout)
$(BUILD)/tests32: $(CORE) $(TEST_SRC) $(MBED_LIB) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DPLAT_SEQ_TEST -DPLAT_FRAM_SIZE=32768u -Itests -Ihost/eve_sim $(filter-out %.h,$^) -o $@ -lm

sim: $(BUILD)/sim
	./$(BUILD)/sim

target:
	$(MAKE) -f port/stm32h5/check.mk

boot:
	$(MAKE) -f boot/boot.mk

clean:
	rm -rf $(BUILD)

# FreeRTOS POSIX run of the real task layer (needs ../ext/FreeRTOS-Kernel)
EXT ?= $(abspath ../ext)
FR   = $(EXT)/FreeRTOS-Kernel
RTOS_SRC = platform/core/scan_engine.c platform/core/plat_cmdq.c platform/core/task_monitor.c \
           platform/core/plat_diag.c platform/core/param.c platform/core/param_store.c platform/core/plat_crc32.c \
           platform/core/plat_sys.c platform/core/modbus.c platform/core/plat_sha256.c platform/core/plat_log.c platform/core/plat_alarm.c platform/core/plat_state.c platform/core/plat_power.c platform/core/plat_cal.c platform/core/plat_service.c platform/core/plat_selftest.c platform/core/plat_boardtest.c platform/core/plat_contract.c platform/core/plat_clock.c platform/core/fw_update.c platform/core/boot_core.c platform/core/fw_pubkey.c platform/core/dev_pubkey.c platform/core/plat_serial.c platform/core/mb_rtu.c platform/core/iob_net.c platform/core/iobus.c platform/core/w5500.c platform/core/iob_w5500.c platform/net/iob_tcp.c platform/net/web_auth.c platform/net/tls.c platform/net/mail.c platform/net/sntp.c platform/net/fw_sig.c platform/ui/ui_app.c \
           platform/bsp/nvm_ram.c platform/bsp/nor_sim.c platform/bsp/fw_flash_sim.c platform/os/plat_tasks_freertos.c platform/os/plat_store_task.c platform/os/plat_iobus_task.c platform/core/plat_role.c platform/net/plat_node.c node/node.c node/node_net.c app_demo/app_demo.c \
           host/rtos_sim/bsp_posix.c host/rtos_sim/rtos_main.c \
           $(FR)/tasks.c $(FR)/list.c $(FR)/queue.c $(FR)/timers.c $(FR)/portable/MemMang/heap_3.c \
           $(FR)/portable/ThirdParty/GCC/Posix/port.c $(FR)/portable/ThirdParty/GCC/Posix/utils/wait_for_event.c
$(BUILD)/rtos_sim: $(RTOS_SRC) $(MBED_LIB) | $(BUILD)
	$(CC) -std=gnu11 -O1 -g -Wall -DPLAT_POSIX_SOCKETS -Iplatform/include -Iplatform/bsp -Inode -Iapp_demo -Ihost/rtos_sim $(MBED_FLAGS) \
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

# ---- the I/O node (node/: the node role's core, formerly the io-node repository) ----
NODE_INC  = -Inode -Inode/drivers -Iport/node_stm32g0 -Itests/node -Iplatform/include -Iplatform/bsp
NODE_SRC  = node/node.c node/node_net.c platform/core/mb_rtu.c platform/core/iob_net.c
NODE_TSRC = tests/node/test_main.c tests/node/test_node.c tests/node/test_net.c tests/node/test_diff.c tests/node/test_drv.c \
            tests/node/sim_hw.c tests/node/host_port.c tests/node/sim_chips.c tests/node/sim_ads124s08.c tests/node/test_temp.c \
            platform/bsp/iob_node_sim.c
NODE_HDRS = $(wildcard node/*.h node/drivers/*.h tests/node/*.h) port/node_stm32g0/node_port.h platform/include/iobus.h platform/include/iob_net.h platform/include/mb_rtu.h
$(BUILD)/node_tests: $(NODE_SRC) $(wildcard node/drivers/*.c) $(NODE_TSRC) $(NODE_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -DPLAT_HOST -DADS124S08_CONFIRMED=1 $(NODE_INC) $(filter %.c,$^) -o $@ -lm
# the node role on a PC (host/nodesim): plat_node.c and the node core over POSIX sockets
NODESIM_SRC = host/nodesim/nodesim_main.c platform/net/plat_node.c node/node.c node/node_net.c platform/core/iob_net.c platform/core/mb_rtu.c
$(BUILD)/nodesim: $(NODESIM_SRC) $(NODE_HDRS) platform/include/plat_node.h | $(BUILD)
	$(CC) $(CFLAGS) -DPLAT_HOST -DPLAT_NODE_DEFAULT_IP=0xC0A8013Du -Inode -Iplatform/include $(NODESIM_SRC) -o $@ -lpthread
nodesim: $(BUILD)/nodesim
node-test: $(BUILD)/node_tests
	./$(BUILD)/node_tests | tail -3

# the platform's host tests against the real node core (tests/node/platform/iob_node_real.c)
REAL_NODE = tests/node/platform/iob_node_real.c node/node.c node/node_net.c tests/node/sim_hw.c
$(BUILD)/tests_real: $(filter-out platform/bsp/iob_node_sim.c,$(CORE)) $(REAL_NODE) $(TEST_SRC) $(MBED_LIB) $(HDRS) $(NODE_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) -DPLAT_SEQ_TEST -Itests -Ihost/eve_sim -Inode -Itests/node $(filter-out %.h,$^) -o $@ -lm
master-real: $(BUILD)/tests_real
	./$(BUILD)/tests_real | tail -2
