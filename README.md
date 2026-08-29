# Nordic nRF Matter Dual Temperature Sensor

![20251223_184336358_iOS](https://github.com/user-attachments/assets/d63748a7-c801-4c82-87ea-a7dafc8251de)

> [!WARNING]
> This is a work in progress. It works, but I'm experiencing network reliability issues with the battery variation.

This is my nRF54L15 Matter Dual Temperature Sensor. 

It supports two NTC thermistor probes, allowing for two temperature readings.

It is compatible with all commercial Matter controllers as it uses the standard Temperature Measurement cluster.

## Battery Life

The firmware has been optimised to reduce power consumption using Matter ICD (Intermittant Connected Device) and LIT (Long Idle Time)

It consumes an average 18µA, which allows for approximately 1.5 years on a standard 240mAh 2032 coin cell battery. 

The cell voltage is reported through the standard Matter PowerSource cluster, so the remaining battery level shows up in the controller alongside the two temperatures. The coin cell drives VDD directly, so the level is read from the SAADC's internal VDD input and costs no extra hardware.

## Firmware

The code is written using the Nordic Connect SDK, v3.3.0.

### TODO

- [x] Implement ADC
- [x] Add two temperature probes
- [x] Basic indicator LED (without DK)
- [x] Press and hold to reset
- [x] User labels to name the probes
- [x] Report the battery level
- [x] Custom cluster reporting both probes from one endpoint

## Hardware

There is a PCB design, which you'll find under the firmware folder.
