# Firmware

## Compiling

This code has been developed against NCS v3.1.0.

There are two variations of the project. One designed for battery and one to be powered via the mains (USB)

To build for the custom PCB, use this command for USB

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DFILE_SUFFIX=minewsemi -DEXTRA_CONF_FILE=prj_usb.conf
```

or for battery

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DFILE_SUFFIX=minewsemi -DEXTRA_CONF_FILE=prj_battery.conf
```

To compile for the nRF54L15-DK, use this command

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DEXTRA_CONF_FILE=prj_battery.conf
```