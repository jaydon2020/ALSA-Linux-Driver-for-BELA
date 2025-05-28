+++
title = "Porting ReSpeaker 2-Mics Pi HAT v2 to BeagleY-AI"
description = ""
date = 2025-05-10
[taxonomies]
tags = ["BeagleY-AI", "ALSA", "TLV320AIC3104"]
+++

# **Porting ReSpeaker 2-Mics Pi HAT v2 to BeagleY-AI**

REVISED MAY 2025

{% warning(title="Warning") %} 
This wiki is wrote for ReSpeaker 2-Mics Pi HAT v2, to distinguish the v1 and v2 device, please refer to [How to Distinguish ReSpeaker 2-Mics Pi HAT Hardware Revisions](https://wiki.seeedstudio.com/how-to-distinguish-respeaker_2-mics_pi_hat-hardware-revisions/).
{% end %}

## **Hardware Overview**  

### 1. **BeagleY-AI**  

![BeagleY-AI](https://docs.beagleboard.org/_images/beagley-ai.webp)

BeagleY® AI is an open-source single board computer designed to simplify the process of building smart human machine interfaces (HMI), adding cameras and high-speed connectivity to a reliable embedded system. It features a powerful 64-bit, quad-core A53 processor, multiple powerful AI accelerators paired with C7x DSPs, integrated 50 GFLOP GPU supporting up to three concurrent display outputs and modern connectivity including USB3.1, PCIe Gen 3, WiFi6 and Bluetooth® Low Energy 5.4.

The board is compatible with a wide range of existing accessories that expand the system functionality such as power over ethernet (PoE), NVMe storage and 5G connectivity.

With its competitive price and user-friendly design, Beagle Y AI provides a positive development experience using BeagleBoard's tried and tested custom Debian Linux image.

### 2. **ReSpeaker 2-Mics Pi HAT v2**

{% note(title="Note") %}
The reSpeaker 2-Mics Pi HAT has been upgraded to version 2.0. The codec chip has been replaced from WM8960 to TLV320AIC3104, enabling support for Raspberry Pi 5 and expanding the sampling rate range from 8 kHz to 96 kHz.
{% end %}

![ReSpeaker](https://media-cdn.seeedstudio.com/media/catalog/product/cache/bb49d3ec4ee05b6f018e93f896b8a25d/0/4/04_8_1.png)

ReSpeaker 2-Mics Pi HAT is a dual-microphone expansion board for Raspberry Pi designed for AI and voice applications. You can build a more powerful and flexible voice product that integrates Amazon Alexa Voice Service, Google Assistant, and so on.

## **Setup Process**

### **1. Bootable SD Card Preparation**

Create a bootable microSD card with latest/recommended OS image for BeagleY-AI. [BeagleY-AI Quick Start](https://docs.beagleboard.org/boards/beagley/ai/02-quick-start.html)

### **2. Kernel Driver Compilation**

{% note(title="Note") %}
The BeagleY-AI typically comes with the tlv320aic31xx codec driver. If the snd-soc-tlv320aic31xx.ko.xz file is missing, try updating and upgrading your system using the command:
sudo apt update && sudo apt upgrade

If the driver still does not exist after updating, follow the steps below to build it from source.
If the file is present, you can skip this step and proceed directly to the “Load Device Tree” section.
{% end %}

```bash
$ cd linux

# default configuration
$ make distclean
$ make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- bb.org_defconfig

# config camera
$ make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- menuconfig
# -> Device Drivers
#   -> Sound card support
#     -> Advance Linux Sound Architecture
#       -> ALSA for SoC audio support
#         -> CODEC drivers
#             -> Texas Instruments TLV320AIC31xx CODECs
#                Set "TLV320AIC31xx" to module,
#                Press "m", save to original name (.config) and exit

$ make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image dtbs modules LOCALVERSION="-ti-arm64-r23" -j$(nproc)
```

Plug in the SD card which burned in” Create a bootable microSD card” to PC.

**Install onto the SD card**:

```bash
$ sudo cp arch/arm64/boot/Image /media/$(users)/BOOT/
$ sudo cp arch/arm64/boot/dts/ti/*.dtbo /media/$(users)/BOOT/overlays/
# you can use "make kernelversion" to check kernel version
$ sudo cp -ra modules/lib/modules/$(make kernelversion)-ti-arm64-r23/ /media/$(users)/rootfs/lib/modules/
$ sync
```

Boot BeagleY-AI with SD card.

## **Device Tree Configuration**  

**Get compiled Device Tree Source (DTS)**:

```bash
# Download the compiled Device Tree Source (DTS) 
$ curl https://raw.githubusercontent.com/jaydon2020/ReSpeaker-2-Mics-Pi-HAT-v2-BeagleY-AI/refs/heads/main/overlays/k3-am67a-beagley-ai-respeaker.dtbo -o k3-am67a-beagley-ai-respeaker.dtbo

# Move the .dtbo file to /boot/firmware/overlays/
$ mv k3-am67a-beagley-ai-respeaker.dtbo /boot/firmware/overlays/
```

**Load Device Tree Configuration**:

Add the overlay to label microSD (default) and append fdtoverlays `/overlays/k3-am67a-beagley-ai-respeaker.dtbo` after the fdt line.
To use the LEDs, you need enable SPI interface first. To enable SPI interface, open the BeagleY-AI `/boot/firmware/extlinux/extlinux.conf`

```bash
$ sudo nano /boot/firmware/extlinux/extlinux.conf

> label microSD (default)
>    kernel /Image
>    append console=ttyS2,115200n8 root=/dev/mmcblk1p3 ro rootfstype=ext4 resume=/dev/mmcblk1p2 rootwait net.ifnames=0 quiet
>    fdtdir /
>    fdt /ti/k3-am67a-beagley-ai.dtb
>    fdtoverlays /overlays/k3-am67a-beagley-ai-respeaker.dtbo /overlays/k3-am67a-beagley-ai-spidev0.dtbo
>    initrd /initrd.img
```

**Restart**:

```bash
sudo reboot -f
```

## **Basic Recording & Playback**

Use `aplay -l` to list the playback device and `arecord -l` to list the recording device.

**Record mono audio (16-bit, 16kHz)**:

```bash
arecord -D plughw:0,0 --format S16_LE --rate 16000 --channels 1 --duration 5 test_mono.wav
```

{% note(title="Note") %}
Why plughw?The ReSpeaker HAT's hardware `(hw:0,0)` natively supports stereo input only.plughw automatically converts the stereo stream to mono by averaging channels.
{% end %}

**Record stereo audio (16-bit, 48kHz)**:

```bash
arecord -D hw:0,0 --format S16_LE --rate 48000 --channels 2 --duration 5 test_stereo.wav  
```

**Playback recorded audio**:

```bash
aplay -D hw:0,0 test_mono.wav
aplay -D hw:0,0 test_stereo.wav
```

**Record Sound with Python**:

```bash
git clone https://github.com/jaydon2020/ReSpeaker-2-Mics-Pi-HAT-v2-BeagleY-AI.git 
cd mic_hat

sudo apt-get install portaudio19-dev libatlas-base-dev
```

We use PyAudio python library to record sound with Python.First, run the following script to get the device index number of ReSpeaker

```bash
python3 recordings/detect_microphone.py
```

To record the sound, open `recording_examples/record_stero.py` file with nano, vim or other text editor and change `RESPEAKER_INDEX = 1` to index number of ReSpeaker on your system. Then run python script `record_stero.py` to make a recording:

```bash
python3 recordings/record_stereo.py
```

## **On-board User LEDs and Button**

Setup

```bash
sudo apt update
sudo apt install python3-libgpiod python3-spidev
```

Run `python interfaces/pixels.py` to test the LEDs.
Run `python interfaces/button.py` to test the button.

## **Full porting process and documentation**

Post on hackster [Porting the ReSpeaker 2-Mics Pi HAT v2 to BeagleY-AI](https://www.hackster.io/jaydon-msia/porting-the-respeaker-2-mics-pi-hat-v2-to-beagley-ai-6dd8f2) with an application example keyword spotting

## **Refrenecs**

1. [https://www.seeedstudio.com/ReSpeaker-2-Mics-Pi-HAT.html](https://www.seeedstudio.com/ReSpeaker-2-Mics-Pi-HAT.html)
2. [https://docs.beagleboard.org/boards/beagley/ai/index.html](https://docs.beagleboard.org/boards/beagley/ai/index.html)