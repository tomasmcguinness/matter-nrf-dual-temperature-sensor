# Nordic nRF Matter Dual Temperature Sensor

![20251223_184336358_iOS](https://github.com/user-attachments/assets/d63748a7-c801-4c82-87ea-a7dafc8251de)

This is my Matter Dual Temperature Sensor. 

It supports two NTC thermistor probes, allowing for two temperature readings.

It is compatible with all commercial Matter controllers as it uses the standard Temperature Measurement cluster.

## Battery Life

The firmware has been optimised to reduce power consumption using Matter ICD (Intermittant Connected Device) and LIT (Long Idle Time)

It consumes approx 18µA, which allows for approximately 1.5 years on a standard 240mAh 2032 coin cell battery. 

## Firmware

The code is written using the Nordic Connect SDK, v3.1.0.

### TODO

- [x] Implement ADC
- [x] Add two temperature probes
- [x] Basic indicator LED (without DK)
- [x] Press and hold to reset
- [ ] User labels to name the probes

## Hardware

There is a PCB design 
