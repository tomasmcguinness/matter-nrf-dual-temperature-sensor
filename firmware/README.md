# Firmware

## Compiling

This code has been developed against NCS v3.3.0. Note that v3.3.0 is a hard requirement: on
v3.1.0 `NRF_SAADC_VDD` resolves to the wrong SAADC input and the battery measurement reads an
external pin instead of the supply rail.

The device is battery powered only. Debug builds need no extra configuration; the `_release`
variation removes logging and the shells.

To build the custom PCB, use this command

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DFILE_SUFFIX=minewsemi
```

or, for a release build

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DFILE_SUFFIX=minewsemi -DEXTRA_CONF_FILE=prj_release.conf
```

To compile for the nRF54L15-DK, use this command

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp
```

## Battery measurement

The coin cell drives VDD directly - there is no LDO and no divider - so the battery voltage is
read from the SAADC's internal VDD input. This costs no extra hardware and draws no quiescent
current. See `boards/nrf54l15dk_nrf54l15_cpuapp*.overlay` for the channel definition and
`src/battery.cpp` for the reader.

The voltage is sampled once shortly after start up and then once a day, and is reported through
the Matter PowerSource cluster on endpoint 0 as `BatVoltage`, `BatPercentRemaining` and
`BatChargeLevel`. The percentage is a linear mapping of the 2000mV - 3000mV CR2032 operating
window.

On the nRF54L15-DK this reads the board's VDD selector rather than a cell, which makes it
convenient for sweeping the thresholds on the bench.

## Regenerating the data model

The ZAP generated sources under `src/zap/zap-generated` are checked in and consumed directly
(`BYPASS_IDL`), so editing `src/zap/template.zap` has no effect until they are regenerated:

```
export PATH=$HOME/ncs/toolchains/43683a87ea/usr/local/lib/python3.12/site-packages/clang_format/data/bin:$PATH
export ZAP_INSTALL_PATH=$HOME/ncs/v3.3.0/modules/lib/matter/.zap-install
M=$HOME/ncs/v3.3.0/modules/lib/matter
python3 $M/scripts/tools/zap/generate.py $PWD/src/zap/template.zap \
    -t $M/src/app/zap-templates/app-templates.json \
    -z $M/src/app/zap-templates/zcl/zcl.json \
    -o $PWD/src/zap/zap-generated
```

Then regenerate `template.matter` alongside it, swapping `app-templates.json` for
`matter-idl-server.json` and adding `--matter-file-name $PWD/src/zap/template.matter`.

Two notes on the environment:

- The templates must come from the **v3.3.0** SDK. v3.3.0's `chip_data_model.cmake` expects
  `CodeDrivenInitShutdown.cpp` and `CodeDrivenCallback.h` in the generated directory, and the
  older templates do not emit them, so the build fails at the configure step.
- `clang-format` must be on the PATH or the generator skips prettifying and the output differs
  from the checked-in files by formatting alone. The version above (17.0.1, from the v3.2.0
  toolchain) is the one that matches the existing files - the v3.3.0 toolchain ships 21.1.8,
  which formats slightly differently.

## Flashing

To flash the firmware, run the following command

```
west flash
```

If the device has protected firmware, like the MinewSemi ME54BS01, you may need to use the
recover flag

```
west flash --recover
```

> [!NOTE]
> Ultimately, the use of protected firmware will be restored. I can only learn some many things at once!
