# Firmware

## Compiling

This code has been developed against NCS v3.1.0.

There are two variations of the project. One designed for battery and one to be powered via the mains (USB). There are also `_release` variations too, that remove logging.

To build the custom PCB, use this command for device that will be powered by mains powered

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DFILE_SUFFIX=minewsemi -DEXTRA_CONF_FILE=prj_wired.conf
```

or for battery powered devices, use this command.

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DFILE_SUFFIX=minewsemi -DEXTRA_CONF_FILE=prj_battery.conf
```

To compile for the nRF54L15-DK, use this command

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DEXTRA_CONF_FILE=prj_battery.conf
```

## Flashing

To flash the firmware, run the following command

```
west flash
```

If the device has protected firmware, like the MinewSemi ME54BS01, you may need to use the recover flag

```
west flash --recover
```

> [!NOTE]
> Ultimately, the use of protected firmware will be restored. I can learn some many things at once!