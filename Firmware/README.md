# Firmware

## Compiling

To build for the PCB that is part of this project, use this command

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DFILE_SUFFIX=minewsemi -Dtemplate_EXTRA_CONF_FILE=prj_release.conf
```

To compile for the nRF54L15-DK, use this command

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp --
```