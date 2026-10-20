# check.mk - compile-check the target sources with arm-none-eabi-gcc against the
# real FreeRTOS kernel, STM32H5 HAL and CMSIS headers (no link). Not the product
# build: that is the STM32CubeIDE project (docs/CUBEMX_SETUP.md).
#   make target EXT=/path/to/ext   (default ../ext next to this repo)
EXT    ?= $(abspath ../ext)
ARMCC  ?= $(EXT)/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-gcc
FR      = $(EXT)/FreeRTOS-Kernel
DEV     = -DSTM32H573xx -DUSE_HAL_DRIVER
CFLAGS  = -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -std=c11 -O2 \
          -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror -ffunction-sections -fdata-sections
LW      = $(EXT)/lwip
LWIP_SRC = $(wildcard $(LW)/src/core/*.c) $(wildcard $(LW)/src/core/ipv4/*.c) $(wildcard $(LW)/src/api/*.c) \
           $(LW)/src/netif/ethernet.c $(LW)/contrib/ports/freertos/sys_arch.c
MBED    = third_party/mbedtls
MBED_SRC = $(wildcard $(MBED)/library/*.c)
INC     = -DMBEDTLS_CONFIG_FILE='"tls_config.h"' -Iplatform/net -I$(MBED)/include -I$(MBED)/library -I$(LW)/src/include -I$(LW)/contrib/ports/freertos/include -Iport/stm32h5/lwip -I$(EXT)/lan8742 \
          -Iplatform/include -Iplatform/bsp -Iapp_demo -Iport/stm32h5 -Iport/stm32h5/check \
          -I$(FR)/include -I$(FR)/portable/GCC/ARM_CM33_NTZ/non_secure \
          -I$(EXT)/stm32h5xx-hal-driver/Inc -I$(EXT)/cmsis-device-h5/Include -I$(EXT)/cmsis_core/CMSIS/Core/Include
SRCS    = platform/core/scan_engine.c platform/core/plat_cmdq.c platform/core/task_monitor.c \
          platform/core/plat_diag.c platform/core/param.c platform/core/param_store.c platform/core/plat_crc32.c \
          platform/core/plat_sys.c platform/core/plat_log.c platform/core/plat_alarm.c platform/core/plat_clock.c platform/core/fw_update.c platform/core/fw_pubkey.c platform/bsp/nvm_ram.c platform/bsp/nor_sim.c \
          platform/os/plat_tasks_freertos.c platform/os/plat_store_task.c \
          port/stm32h5/bsp_stm32h5.c port/stm32h5/nvm_fram_spi.c port/stm32h5/nvm_fram_i2c.c port/stm32h5/nor_spi.c port/stm32h5/fw_flash_stm32h5.c \
          platform/core/modbus.c platform/core/plat_sha256.c platform/net/mbtcp.c platform/net/http_util.c \
          platform/net/http_api.c platform/net/http_server.c platform/net/web_auth.c platform/net/web_page.c platform/net/log_csv.c platform/net/tls.c platform/net/mail.c platform/net/sntp.c platform/net/fw_sig.c \
          platform/os/plat_net_tasks.c platform/os/plat_ui_task.c platform/ui/eve.c platform/ui/eve_fonts.c platform/ui/ui.c port/stm32h5/eve_bus_stm32h5.c port/stm32h5/lwip/plat_net_lwip.c port/stm32h5/lwip/ethernetif.c \
          $(EXT)/lan8742/lan8742.c $(LWIP_SRC) $(MBED_SRC) \
          app_demo/app_demo.c port/stm32h5/check/check_main.c \
          $(FR)/tasks.c $(FR)/list.c $(FR)/queue.c $(FR)/timers.c \
          $(FR)/portable/GCC/ARM_CM33_NTZ/non_secure/port.c $(FR)/portable/GCC/ARM_CM33_NTZ/non_secure/portasm.c
OUT     = build/target
OBJS    = $(addprefix $(OUT)/,$(notdir $(SRCS:.c=.o)))
vpath %.c $(sort $(dir $(SRCS)))

all: $(OBJS)
	@echo "target compile check OK: $(words $(OBJS)) files"
	@$(dir $(ARMCC))arm-none-eabi-size $(filter-out $(OUT)/tasks.o $(OUT)/list.o $(OUT)/queue.o $(OUT)/timers.o $(OUT)/port.o $(OUT)/portasm.o,$(OBJS))
THIRD = $(addprefix $(OUT)/,$(notdir $(LWIP_SRC:.c=.o))) $(OUT)/lan8742.o $(addprefix $(OUT)/,$(notdir $(MBED_SRC:.c=.o)))
$(THIRD): CFLAGS := $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS))
$(OUT)/%.o: %.c | $(OUT)
	$(ARMCC) $(CFLAGS) $(DEV) $(INC) -c $< -o $@
$(OUT):
	mkdir -p $(OUT)

# Link check: full ELF with startup, system file and the HAL modules the BSP uses,
# against the ST template linker script patched with noinit_section.ld.
HALSRC  = $(addprefix $(EXT)/stm32h5xx-hal-driver/Src/stm32h5xx_hal, .c _cortex.c _gpio.c _rcc.c _rcc_ex.c _rtc.c _rtc_ex.c _iwdg.c _pwr.c _pwr_ex.c _spi.c _spi_ex.c _i2c.c _i2c_ex.c _dma.c _dma_ex.c _eth.c _eth_ex.c _rng.c _rng_ex.c _flash.c _flash_ex.c _icache.c)
LSRC    = $(EXT)/cmsis-device-h5/Source/Templates/system_stm32h5xx.c $(HALSRC) $(FR)/portable/MemMang/heap_4.c
LOBJS   = $(OBJS) $(addprefix $(OUT)/,$(notdir $(LSRC:.c=.o))) $(OUT)/startup_stm32h573xx.o
vpath %.c $(sort $(dir $(LSRC)))
$(addprefix $(OUT)/,$(notdir $(LSRC:.c=.o))): CFLAGS := $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS))
vpath %.s $(EXT)/cmsis-device-h5/Source/Templates/gcc
LDT     = $(EXT)/cmsis-device-h5/Source/Templates/gcc/linker/STM32H573xx_FLASH.ld

link: $(OUT)/cems_check.elf
$(OUT)/linker.ld: $(LDT) port/stm32h5/noinit_section.ld | $(OUT)
	sed '/^  ._user_heap_stack :/e cat port/stm32h5/noinit_section.ld' $(LDT) > $@
$(OUT)/startup_stm32h573xx.o: startup_stm32h573xx.s | $(OUT)
	$(ARMCC) -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -c $< -o $@
$(OUT)/cems_check.elf: $(LOBJS) $(OUT)/linker.ld
	$(ARMCC) -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -T$(OUT)/linker.ld \
	  -Wl,--gc-sections -Wl,-Map=$(OUT)/cems_check.map --specs=nano.specs --specs=nosys.specs $(LOBJS) -o $@
	$(dir $(ARMCC))arm-none-eabi-size $@
