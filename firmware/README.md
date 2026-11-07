# Firmware

## Compiling

This code has been developed against NCS v3.3.0. Note that v3.3.0 is a hard requirement: on
v3.1.0 `NRF_SAADC_VDD` resolves to the wrong SAADC input and the battery measurement reads an
external pin instead of the supply rail.

The device is battery powered only. Debug builds need no extra configuration; the `_release`
variation removes logging and the shells.

To build the custom PCB, use this command. This will include logging.

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

## Four probe build (Seeed XIAO nRF52840)

The same application builds for a Seeed XIAO nRF52840 reading **four** NTC probes, powered
over USB. The data model is `src/zap_quad/template.zap`: endpoints 1-4 each carry a
`TemperatureMeasurement` cluster labelled "Probe 1" to "Probe 4", endpoint 0's PowerSource is
**wired** (DC, "USB") rather than battery, and there is no ICD Management cluster.

```
west build -p -b xiao_ble -d build_xiao
west flash -d build_xiao
```

### Wiring

Each probe is a divider from the XIAO's 3V3 pin:

```
3V3 --[ 10k 0.1% ]--+------> A0 / A1 / A2 / A3
                    |
                   NTC   (2-pin screw terminal)
                    |
GND ----------------+------- 100 nF from the A-pin node to GND
```

| Probe | Pin | nRF52840 |
|---|---|---|
| 1 | A0 | P0.02 / AIN0 |
| 2 | A1 | P0.03 / AIN1 |
| 3 | A2 | P0.28 / AIN4 |
| 4 | A3 | P0.29 / AIN5 |
| Factory reset button (optional) | D6 to GND | P1.11 |

The indicator is the XIAO's green LED. The series resistor must match the NTC's 25 C
resistance (`SERIESRESISTOR`, `THERMISTORNOMINAL` and `BCOEFFICIENT` in `src/app_task.cpp`).

### How it differs from the coin cell boards

- **Ratiometric reading.** There is no `battery` channel and no `probe_N_divider_power` node in
  `boards/xiao_ble.overlay`. The dividers sit on 3V3 permanently and each channel uses the VDD/4
  reference at gain 1/4, so full scale is the divider's own supply and
  `Rt = R * raw / (4096 - raw)`. The supply is never measured and regulator drift cancels.
  `src/app_task.cpp` picks the method from the devicetree: a board with a `battery` channel keeps
  the measured-supply method.
- **Always on.** ICD is off and it runs as a full Thread device (FTD), so readings reach the
  controller as soon as they are taken - every minute, all four probes sampled in one pass.
- **USB console.** Logs and the Matter shell are on the USB CDC ACM port (deferred logging),
  as well as RTT.

### Flash layout and programming

The XIAO ships with the Adafruit UF2 bootloader, whose layout reserves 156 kB for a SoftDevice
and leaves no room for MCUboot or Matter factory data. This build replaces it:

| Partition | Address | Size |
|---|---|---|
| MCUboot | 0x00000 | 32 kB |
| Application (with MCUboot header) | 0x08000 | 956 kB |
| Factory data | 0xf7000 | 4 kB |
| Settings | 0xf8000 | 32 kB |
| OTA secondary slot | QSPI 0x000000 | 956 kB |

It is defined in `pm_static_xiao_ble_nrf52840.yml` and mirrored in
`boards/xiao_ble_partitions.dtsi` (used by both the app and MCUboot); keep the two in step.

Program it over SWD, using the pads on the underside of the XIAO (SWDIO, SWCLK, GND, 3V3). An
nRF DK's "Debug out" header works as the probe. **This erases the UF2 bootloader**; to go back
to drag-and-drop programming, reflash the Adafruit nRF52 bootloader. After the first flash,
updates can go over Matter OTA (`build_xiao/dfu_application.zip`).

To regenerate the four probe data model after editing it:

```
west zap-generate -z src/zap_quad/template.zap -o src/zap_quad/zap-generated
```

(with the same environment as described under "Regenerating the data model").

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

## The dual temperature cluster

Alongside the two standard `TemperatureMeasurement` clusters on endpoints 1 and 2, endpoint 3
carries a manufacturer-specific cluster that reports both probes together, so a single read or
subscription returns a pair of readings taken in the same sampling pass.

| | |
|---|---|
| Cluster | `DualTemperatureMeasurement`, code `0xFFF1FC01` |
| Device type | `dual-temperature-sensor`, `0xFFF10001` |
| `Probe1MeasuredValue` | `0xFFF10000`, nullable int16s, 0.01 degC |
| `Probe2MeasuredValue` | `0xFFF10001`, nullable int16s, 0.01 degC |

The definition lives in `src/zap/DualTemperatureCluster.xml`; `src/zap/zcl.json` is the
project-local data model definition that adds it to the SDK's cluster list. The cluster has no
commands and no events, and `AppTask::SensorMeasureHandler()` writes both attributes from the
same pair of readings it gives to endpoints 1 and 2.

The high half of the cluster code is the vendor ID. This uses `0xFFF1`, the CSA test vendor,
which is also what the build falls back to because `CONFIG_CHIP_DEVICE_VENDOR_ID` is unset. That
is fine for a self-built device but is not certifiable - a real vendor ID has to be set in both
places, and the attribute IDs re-prefixed to match.

Endpoints 0, 1 and 2 are untouched, so controllers that know nothing about the custom cluster
still see the two temperature sensors and the battery exactly as before.

## Regenerating the data model

The ZAP generated sources under `src/zap/zap-generated` are checked in and consumed directly
(`BYPASS_IDL`), so editing `src/zap/template.zap` has no effect until they are regenerated.

Because the data model contains a manufacturer-specific cluster, generation has to run in
**full** mode: the app-common accessors for `DualTemperatureMeasurement` do not exist in the
SDK's prebuilt `zzz_generated/app-common`, so they are generated here instead and
`CMakeLists.txt` points `CHIP_APP_ZAP_DIR` at `src/zap/zap-generated` to override the SDK copy.
The cluster is also named in the `EXTERNAL_CLUSTERS` argument to `ncs_configure_data_model()`,
which tells the build not to look for an SDK-side server implementation.

Edit the data model in the ZAP GUI:

```
west zap-gui -j src/zap/zcl.json
```

Then regenerate:

```
export PATH=$HOME/ncs/toolchains/43683a87ea/usr/local/lib/python3.12/site-packages/clang_format/data/bin:$PATH
west zap-generate --full
```

`--full` clears and rewrites the whole of `src/zap/zap-generated` (including the `app-common`,
`clusters` and `devices` subtrees) and regenerates `src/zap/template.matter`.

If the cluster XML is ever renamed or a second custom cluster is added, regenerate `zcl.json`
first:

```
west zap-append -o src/zap/zcl.json --clusters src/zap/DualTemperatureCluster.xml
```

Three notes on the environment:

- Both commands need a west workspace. Run them from inside the NCS toolchain
  (`nrfutil sdk-manager toolchain launch --shell --ncs-version v3.3.0`) with
  `ZEPHYR_BASE=$HOME/ncs/v3.3.0/zephyr` exported, otherwise west cannot find its extension
  commands.
- The templates must come from the **v3.3.0** SDK. v3.3.0's `chip_data_model.cmake` expects
  `CodeDrivenInitShutdown.cpp` and `CodeDrivenCallback.h` in the generated directory, and the
  older templates do not emit them, so the build fails at the configure step.
- `clang-format` must be on the PATH or the generator skips prettifying and the output differs
  from the checked-in files by formatting alone. The version above (17.0.1, from the v3.2.0
  toolchain) is the one that matches the existing files - the v3.3.0 toolchain ships 21.1.8,
  which formats slightly differently.

`src/zap/zcl.json` and `src/zap/template.zap` both hold paths into the SDK that are relative to
this checkout, so they assume the NCS workspace sits at `~/ncs/v3.3.0`.

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
