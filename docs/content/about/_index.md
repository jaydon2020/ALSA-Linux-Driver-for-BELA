+++
title = "serene::about"
description = "Demo collections page of zola-theme-serene"
template = "prose.html"

[extra]
title = "About"
subtitle = "Project description"
reaction = true
+++

## ALSA linux driver for Bela cape

The Bela cape rev C comes with a ES9080Q 8-channel DAC and a TLV320AIC3104 stereo codec. The former has no support on Linux, while the latter is well supported. This project involves: - writing a driver for the ES9080Q, which interacts with existing McASP and DMA drivers - have the ES9080Q and TLV320AIC3104 show up as a single ALSA device with 2 inputs and 10 outputs. Explore whether this can happen via a simple wrapper in the device tree or requires a dedicated driver.

Goal: Upstream support of ES9080Q; simultaneous access to ES9080Q and TLV320AIC3104 via ALSA \
Hardware Skills: basic wiring, logic analyzer \
Software Skills: `C` or `Rust`, `Linux` \
Possible Mentors: [Giulio Moro](https://forum.beagleboard.org/u/giuliomoro) \
Upstream Repository: [Design files for the Bela cape Rev C](https://github.com/BelaPlatform/bela-hardware/tree/master/cape/bela_cape_rev_C3) \
References: [The Bela repo shows how to access these devices with a custom driver and DMA running on the PRU](https://github.com/BelaPlatform/Bela/blob/master/pru/pru_rtaudio_irq.p). \
Proposal: [Submitted Proposals](https://github.com/jaydon2020/ALSA-Linux-Driver-for-BELA/blob/main/proposal/ALSA-Drivers-for-Bela-Project-Proposal.pdf)