# Battery Measurement Test Firmware

A bench-test application used to prove out battery measurement on the nRF54L15 before the code
goes into the shipping firmware under `firmware/`. It is not a product build - there is no
temperature sensing.

What it does:

- Reads the supply voltage over the SAADC every 5 seconds.
- Publishes it through the Matter Power Source cluster on endpoint 0
  (`BatVoltage`, `BatPercentRemaining`, `BatChargeLevel`, `BatPresent`).
- Blinks the indicator LED through the pairing and commissioning states.
- Lights the LED while the button is held, triggers ICD `UserActiveMode` on press, and factory
  resets when held for 10 seconds.

## Hardware

| Function | MinewSemi ME54BS01 | nRF54L15 DK |
|---|---|---|
| Indicator LED | P2.10, active high | P2.09, active high |
| Button | P1.13, active high, pull-down | P1.13, active low, pull-up |
| Battery sense | SAADC internal VDD input | SAADC internal VDD input |

The coin cell drives VDD directly - there is no LDO and no divider - so the SAADC internal VDD
input reads the cell voltage with no extra components and no quiescent draw.

The nRF54L internal reference is 900 mV and the channel uses gain 1/4, putting full scale at
3.6 V. Note that nRF54L15 has no 1/6 gain (only 2, 1, 2/3, 2/4, 2/5, 2/6, 2/7 and 2/8), so
`ADC_GAIN_1_6` fails channel setup with `-EINVAL`.

## Compiling

Requires **NCS v3.3.0**. `NRF_SAADC_VDD` resolves to the wrong SAADC input on v3.1.0, where the
value is 9 (an external AIN) rather than the 128 the nrfx internal-VDD selector expects.

For the custom MinewSemi PCB:

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp -- -DFILE_SUFFIX=minewsemi
```

For the nRF54L15 DK:

```
west build -p -b nrf54l15dk/nrf54l15/cpuapp
```

Unlike `firmware/`, no `EXTRA_CONF_FILE` is needed - there is only one data model, so the ZAP
path lives directly in `prj.conf`.

## Flashing

```
west flash
```

If the device has protected firmware, like the MinewSemi ME54BS01:

```
west flash --recover
```

## Logs

Logging is over SEGGER RTT. The battery voltage and percentage are logged on every measurement
cycle, so RTT is the primary bench readout.

The ME54BS01 has no UART and every UART instance is disabled in its overlay, so on that board
the console moves to RTT and the shell is switched off entirely - its only possible backend
would be RTT, and the RTT log backend already owns that channel. The DK build keeps the Matter
shell on its UART.

## Partitioning

MCUboot, the partition manager layouts and the factory data partition are all carried over
from `firmware/` unchanged, so the flash layout matches the shipping firmware. This is not
because the test app needs DFU - it does not - but because the Matter OTA requestor in the data
model and the factory data generator both require a Partition Manager layout to build at all.

## Regenerating the data model

`cmake/data_model.cmake` passes `BYPASS_IDL`, so the contents of `src/zap/zap-generated/` are
consumed as-is and the ZAP tool is never run during the build. After editing
`src/zap/template.zap`, regenerate both the IDL and the C++ by hand:

```
export ZAP_INSTALL_PATH=~/ncs/v3.3.0/modules/lib/matter/.zap-install
ZAP=~/ncs/v3.3.0/modules/lib/matter/scripts/tools/zap/generate.py

python3 $ZAP "$PWD/src/zap/template.zap" -o "$PWD/src/zap/zap-generated" \
    -m "$PWD/src/zap/template.matter"
python3 $ZAP "$PWD/src/zap/template.zap" -o "$PWD/src/zap/zap-generated" \
    -t ~/ncs/v3.3.0/modules/lib/matter/src/app/zap-templates/app-templates.json
```

The second invocation must produce eight files. Paths passed to `generate.py` must be absolute -
relative ones are resolved against the Matter SDK root, not the working directory.

`generate.py` rewrites `template.zap` in place. The first run after an SDK version bump also
applies a one-off migration pass, which on the move to v3.3.0 wanted to add an `ma_powersource`
endpoint 1 alongside the Power Source cluster on endpoint 0. That was declined - deleting the
endpoint again sticks, since the migration does not re-run once the file has been rewritten.
Diff `template.zap` and `template.matter` after any regeneration that follows an SDK bump.

## Data model

Endpoint 0 only: `ma_rootdevice` + `ma_otarequestor`, carrying Power Source and IcdManagement.
There is no application endpoint, so `Descriptor::PartsList` is empty. That is fine for
`chip-tool`, but some commercial controllers may decline to commission a node with no
application device type. If that happens, add an endpoint in the ZAP file - no code change is
needed.
