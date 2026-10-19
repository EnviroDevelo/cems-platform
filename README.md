# CEMS Core Platform firmware

Shared firmware platform for the CEMS Supervisor instrument family (STM32H5, FreeRTOS,
lwIP, Riverdi EVE4). It runs the translated Valve Manager / Multiplexor program as a
PLC-style scan and provides the services around it.

- `docs/PLATFORM.md`: structure, task priorities, the scan contract, integration rules, decisions needed
- `docs/CUBEMX_SETUP.md`: bringing it up on the STM32H573I-DK in STM32CubeIDE

## Host builds (Linux/macOS/WSL, gcc)

```
make test      # unit tests (1,048 checks)
make sim       # PC simulation of the demo application
make rtos      # the real FreeRTOS task layer on the FreeRTOS POSIX port (needs EXT=path with FreeRTOS-Kernel)
make target    # compile-check for STM32H5 with arm-none-eabi-gcc (needs EXT, see port/stm32h5/check.mk)
```

The product build is the STM32CubeIDE project. These host builds are for testing
outside the IDE.
