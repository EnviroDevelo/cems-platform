# CEMS Core Platform firmware

Shared firmware platform for the CEMS Supervisor instrument family (STM32H5, FreeRTOS,
lwIP, Riverdi EVE4). It runs the translated Valve Manager / Multiplexor program as a
PLC-style scan and provides the services around it.

- `docs/PLATFORM.md`: structure, task priorities, the scan contract, integration rules, decisions needed
- `docs/CUBEMX_SETUP.md`: bringing it up on the STM32H573I-DK in STM32CubeIDE

## Host builds (Linux/macOS/WSL, gcc)

```
make test      # unit tests (with both FRAM memory maps; includes 400 random power cuts on the log)
make sim       # PC simulation of the demo application
make netsim    # the platform on real sockets: Modbus TCP on 1502, web on http://127.0.0.1:8080/
               # (FRAM and log flash are files: netsim_fram.bin, netsim_nor.bin)
               # (type "remote" / "local" on stdin to act as the touchscreen)
make rtos      # the real FreeRTOS task layer on the FreeRTOS POSIX port (needs EXT=path with FreeRTOS-Kernel)
make target    # compile-check for STM32H5 with arm-none-eabi-gcc (needs EXT, see port/stm32h5/check.mk)
tools/check_target.sh   # compile and link the target in five FRAM/NOR configurations
```

Integration tests (with netsim built): `python3 tests/integration/net_test.py` (needs
pymodbus on PYTHONPATH), `python3 tests/integration/fw_test.py` (firmware update and
rollback) and `python3 tests/integration/web_ui_test.py` (needs Python Playwright and
Chromium).

Firmware update files: `tools/mkfwimage.py build.bin cems-x.y.z.cfw --version x.y.z`.

The product build is the STM32CubeIDE project. These host builds are for testing
outside the IDE.
