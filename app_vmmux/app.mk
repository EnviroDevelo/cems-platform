# app.mk - host builds of the VM/MUX application (app_vmmux) on the core platform.
# Run from the repository root:
#   make -f app_vmmux/app.mk vm-test      Stage 4 regression + application tests
#   make -f app_vmmux/app.mk vm-netsim    the platform's networked simulation with this app
#                                         (Modbus TCP on 1502, web on 8080)
# Uses the platform Makefile's flags and source lists unchanged.
include Makefile

VM       = app_vmmux
VM_INC   = -I$(VM)/include -I$(VM)/faithful
VM_SRC   = $(wildcard $(VM)/faithful/*_faithful.c) $(wildcard $(VM)/src/*.c)
VM_HDRS  = $(wildcard $(VM)/include/*.h $(VM)/faithful/*.h)

# The app's own sources also get -Wundef: a vm_config.h switch tested in a file that
# does not include it would otherwise read as 0 without a word.
$(BUILD)/vm_undef.ok: $(VM_SRC) $(VM_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(VM_INC) -Wundef -fsyntax-only $(VM_SRC)
	touch $@

$(BUILD)/vm_tests: $(BUILD)/vm_undef.ok $(CORE) $(VM_SRC) $(VM)/tests/vm_tests.c host/eve_sim/eve_sim.c host/eve_sim/zinflate.c $(MBED_LIB) $(HDRS) $(VM_HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(CPPFLAGS) $(VM_INC) -Itests -Ihost/eve_sim $(filter-out %.h %.ok,$^) -o $@ -lm

# netsim_main.c is written for the demo app; the define points it at this one
VM_NETSIM_SRC = $(filter-out platform/bsp/bsp_host.c,$(CORE)) $(VM_SRC) platform/net/http_server.c host/netsim/netsim_main.c $(MBED_LIB)
$(BUILD)/vm_netsim: $(VM_NETSIM_SRC) $(HDRS) $(VM_HDRS) | $(BUILD)
	$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
	  -DPLAT_HOST -DPLAT_HOST_THREADS -Iplatform/include -Iplatform/bsp -Iapp_demo $(MBED_FLAGS) $(VM_INC) \
	  '-Dapp_demo_plat=(*vmmux_app())' $(filter-out %.h,$^) -o $@ -lm -lpthread

.PHONY: vm-test vm-netsim vm-stage4
vm-stage4:
	sh $(VM)/stage4_tests/run.sh $(BUILD)/stage4
vm-test: vm-stage4 $(BUILD)/vm_tests
	./$(BUILD)/vm_tests
vm-netsim: $(BUILD)/vm_netsim
	./$(BUILD)/vm_netsim
