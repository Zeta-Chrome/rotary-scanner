# RotaryScanner

An embedded Linux 3D object scanner. An object sits on a servo-driven turntable, an ESP32-CAM streams JPEG frames to a Raspberry Pi 3B+ over SPI, a custom V4L2 kernel driver exposes the camera as `/dev/videoN`, and a reconstruction pipeline turns the captured views into a 3D point cloud.

## Overview

- **Bring-up:** U-Boot, kernel, and device tree for the Raspberry Pi 3B+
- **Firmware:** ESP32 camera capture and SPI slave transmission
- **Kernel driver:** An SPI-attached V4L2 capture device using videobuf2
- **Motion control:** Servo turntable with known angular positions
- **Computer vision:** Camera calibration and point cloud reconstruction

## Features

- Custom Linux BSP for Raspberry Pi 3B+ (device tree, U-Boot, kernel)
- ESP32-CAM streaming JPEG frames over SPI (Pi is master, ESP32 is slave)
- Frame-ready interrupt line to avoid polling
- `espcam-spi` V4L2 driver: one chip-select maps to one `/dev/videoN`
- Fixed exposure, gain, and white balance for consistent frames
- servo-driven turntable with step-accurate angle tracking
- Checkerboard camera calibration
- Point cloud output (PLY) viewable in Open3D 

## Hardware

| Item | Notes |
|------|-------|
| Raspberry Pi 3B+ | Main board, runs custom BSP |
| ESP32-CAM (AI-Thinker) | OV2640 camera |
| servo motor + driver | MG90S |
| Turntable platter | Flat disc fixed to the motor shaft, matte non-slip surface |
| Lighting | Bright, diffuse, and stable (LED panel or two lights) |
| Backdrop | Uniform matte black or white |
| Power | Separate supply for the servo. Stable 5 V for the ESP32-CAM |
| Misc | Jumper wires (keep SPI wires short), checkerboard print, USB-UART adapter for flashing the ESP32 |
