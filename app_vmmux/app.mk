# app.mk - host builds of the VM/MUX application (app_vmmux) on the core platform.
# Run from the repository root:
#   make -f app_vmmux/app.mk vm-test      Stage 4 regression + application tests
#   make -f app_vmmux/app.mk vm-nettest   end-to-end test on real sockets (web page API + Modbus)
#   make -f app_vmmux/app.mk vm-webtest   the application's screens on the web page (headless Chromium)
#   make netsim APP=app_vmmux             the platform's networked simulation with this app
#                                         (Modbus TCP on 1502, web on 8080; app_vars.mk, R1)
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

# the networked simulation with this application (platform Makefile, app_vars.mk + vm_select.c)
VM_NETSIM = $(BUILD)/netsim-app_vmmux
$(VM_NETSIM): $(VM_SRC) $(VM_HDRS) $(HDRS)
	$(MAKE) --no-print-directory APP=app_vmmux $@

.PHONY: vm-test vm-stage4 vm-nettest vm-webtest
vm-stage4:
	sh $(VM)/stage4_tests/run.sh $(BUILD)/stage4
vm-test: vm-stage4 $(BUILD)/vm_tests
	./$(BUILD)/vm_tests
# real sockets: web API + Modbus TCP against vm_netsim (python3, no extra packages)
vm-nettest: $(VM_NETSIM)
	python3 $(VM)/tests/integration/vm_net_test.py
# the screens on the web page in headless Chromium (Python Playwright; screenshots build/vmweb_*.png)
vm-webtest: $(VM_NETSIM)
	python3 $(VM)/tests/integration/vm_web_ui_test.py
