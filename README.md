# Project Velliangiri

Mainline **Linux 6.12** kernel port and open-source (`GPL-2.0-only`) hardware drivers for the **S5L8947X** (32nm single-core ARMv7 Cortex-A9) SoC platform.

## Hardware Support Scope

- **Core SoC**: Interrupt controller, watchdog timer, clocksource, and power/clock management (`PMGR`)
- **Networking & Connectivity**: 10/100 Ethernet MAC, USB 2.0 OTG (`dwc2`), and EHCI HSIC host
- **Storage & DMA**: Central DMA controller (`CDMA`) and NAND flash memory interface (`FMI`)
- **Peripherals**: I2C controllers, PMU regulator/MFP, UART serial interfaces, HDMI simpledrm framebuffer, and I2S/SPDIF audio

## License

All kernel code and drivers in this repository are licensed under `GPL-2.0-only`. See [LICENSE](LICENSE) for details.
