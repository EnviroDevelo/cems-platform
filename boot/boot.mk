# boot.mk - the first-stage bootloader (boot.h, docs/BOOTLOADER.md): a real, linked
# image for the STM32H573, built with arm-none-eabi-gcc (no CubeIDE project needed).
#   make boot EXT=/path/to/ext   ->  build/boot/cems_boot.elf, .bin, .hex
# It must fit PLAT_BOOT_SIZE (32 KB); the link fails if it does not.
EXT    ?= $(abspath ../ext)
ARMCC  ?= $(EXT)/xpack-arm-none-eabi-gcc-13.3.1-1.1/bin/arm-none-eabi-gcc
BIN     = $(dir $(ARMCC))arm-none-eabi-
BOOT_SIZE ?= 0x8000
ARCH    = -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard
DEV     = -DSTM32H573xx -DUSE_HAL_DRIVER -DPLAT_BOOT_SIZE=$(BOOT_SIZE)u
CFLAGS  = $(ARCH) -std=c11 -Os -Wall -Wextra -Wshadow -Wconversion -Wno-sign-conversion -Werror \
          -ffunction-sections -fdata-sections
INC     = -Iboot -Iplatform/include -Iapp_demo \
          -I$(EXT)/stm32h5xx-hal-driver/Inc -I$(EXT)/cmsis-device-h5/Include -I$(EXT)/cmsis_core/CMSIS/Core/Include
SRCS    = boot/boot_main.c platform/core/boot_core.c platform/core/plat_sha256.c platform/core/plat_crc32.c \
          platform/core/fw_pubkey.c
HALSRC  = $(addprefix $(EXT)/stm32h5xx-hal-driver/Src/stm32h5xx_hal, .c _flash.c _flash_ex.c _pka.c) \
          $(EXT)/cmsis-device-h5/Source/Templates/system_stm32h5xx.c
OUT    ?= build/boot
OBJS    = $(addprefix $(OUT)/,$(notdir $(SRCS:.c=.o)))
HOBJS   = $(addprefix $(OUT)/,$(notdir $(HALSRC:.c=.o)))
vpath %.c $(sort $(dir $(SRCS) $(HALSRC)))
vpath %.s $(EXT)/cmsis-device-h5/Source/Templates/gcc
LDT     = $(EXT)/cmsis-device-h5/Source/Templates/gcc/linker/STM32H573xx_FLASH.ld

.PHONY: all boot
all: $(OUT)/cems_boot.bin
boot: all
$(HOBJS): CFLAGS := $(filter-out -Werror -Wconversion -Wshadow -Wextra,$(CFLAGS))
$(OUT)/%.o: %.c | $(OUT)
	$(ARMCC) $(CFLAGS) $(DEV) $(INC) -c $< -o $@
$(OUT)/startup_stm32h573xx.o: startup_stm32h573xx.s | $(OUT)
	$(ARMCC) $(ARCH) -c $< -o $@
# ST's template, cut down: flash = the bootloader's sectors, RAM = the top 32 KB of SRAM
# (the application's .noinit block lies lower), small stack, no heap
$(OUT)/boot.ld: $(LDT) | $(OUT)
	sed -e 's/^  RAM    (xrw)    : ORIGIN = 0x20000000,   LENGTH = 640K/  RAM (xrw) : ORIGIN = 0x20098000, LENGTH = 32K/' \
	    -e 's/^  FLASH    (rx)    : ORIGIN = 0x8000000,   LENGTH = 2048K/  FLASH (rx) : ORIGIN = 0x8000000, LENGTH = $(BOOT_SIZE)/' \
	    -e 's/^_Min_Heap_Size = .*/_Min_Heap_Size = 0;/' -e 's/^_Min_Stack_Size = .*/_Min_Stack_Size = 0x2000;/' $(LDT) > $@
	grep -q 'ORIGIN = 0x20098000' $@ && grep -q 'LENGTH = $(BOOT_SIZE)' $@
$(OUT)/cems_boot.elf: $(OBJS) $(HOBJS) $(OUT)/startup_stm32h573xx.o $(OUT)/boot.ld
	$(ARMCC) $(ARCH) -T$(OUT)/boot.ld -Wl,--gc-sections -Wl,-Map=$(OUT)/cems_boot.map --specs=nano.specs --specs=nosys.specs \
	  $(OBJS) $(HOBJS) $(OUT)/startup_stm32h573xx.o -o $@
	$(BIN)size $@
$(OUT)/cems_boot.bin: $(OUT)/cems_boot.elf
	$(BIN)objcopy -O binary $< $@
	$(BIN)objcopy -O ihex $< $(OUT)/cems_boot.hex
	@echo "bootloader: $$(stat -c %s $@) of $$(($(BOOT_SIZE))) bytes"
$(OUT):
	mkdir -p $(OUT)
