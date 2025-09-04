> [!NOTE]
> I'm in the process of building this project, so it's nothing more than the template sample

## Environment

To setup the NCS environment in WSL

```
nrfutil sdk-manager toolchain launch --ncs-version v3.1.0 --shell
```

then run

```
. ~/ncs/v3.1.0/zephyr/zephyr_env.sh
```

## Compiling

To compile the application for the nRF52840dk

```
west build -b nrf52840dk/nrf52840 -d nrf52840dk -- -DCONF_FILE=prj_release.conf
```

```
west flash -d nrf52840dk
```