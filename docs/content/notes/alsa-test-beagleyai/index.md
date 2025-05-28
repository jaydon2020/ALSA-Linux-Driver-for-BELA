+++
title = "Measuring ALSA Latency on a BeagleY‑ai with Xenomai4‑EVL"
description = "Building on our Preempt-RT kernel for μs-level determinism"
date = 2025-05-29
[taxonomies]
tags = ["BeagleY-AI", "ALSA", "Xenomai4"]
+++


## Introduction

Low‑latency audio processing is critical for embedded systems that require real‑time performance, such as voice interfaces, digital signal processing (DSP), and interactive media applications. In this post, we demonstrate how to compile and test the Advanced Linux Sound Architecture (ALSA) library on a BeagleY‑ai running a Xenomai4‑EVL 6.6.69 kernel, and analyze the latency characteristics using the provided ALSA latency test.

## Background

* **Platform**: BeagleY‑ai development board
* **Kernel**: Xenomai4‑EVL 6.6.69 [Integrating Xenomai 4 on BeagleY-AI: Hard Real-Time Linux](../xenomai4-beagleyai/)
* **Sound Card**: ReSpeaker 2-Mics Pi HAT v2 [Porting ReSpeaker 2-Mics Pi HAT v2 to BeagleY-AI](../respeaker-beagleyai/)
* **ALSA version**: Building from source at v1.2.14 (upgrading from the distribution package 1.2.8)
* **Realtime scheduler**: Round Robin (RR) at high priority to minimize scheduling jitter

Real‑time extensions like Xenomai’s EVL mode allow latency‑critical tasks to bypass much of the Linux scheduler’s unpredictability. ALSA’s test suite (`alsa-lib/test/latency`) can then measure round‑trip latency in frames and microseconds.

## Environment Setup

1. **Update and install prerequisites**

   ```bash
   sudo apt update
   sudo apt install automake libtool git build-essential
   ```

2. **Clone and checkout ALSA‑lib v1.2.14**

   ```bash
   git clone https://github.com/alsa-project/alsa-lib.git
   cd alsa-lib
   git checkout v1.2.14 -b v1.2.14
   ./gitcompile
   ```

3. **Build the latency test**

   ```bash
   cd alsa-lib/test
   make latency
   ```

Ensure that the EVL kernel module is loaded and that your audio device (`hw:0,0`) is available.

## Running the Latency Test

We invoke the test with the following parameters:

* **Playback and capture targets**: `hw:0,0`
* **Sample rate**: 44.1 kHz
* **Buffer size**: 64 frames
* **Period size**: 64 frames (first run), then 32 frames (second run)
* **Mode**: Polling, non-blocking
* **Scheduler**: Round Robin at priority 99

```bash
sudo ./latency -P hw:0,0 -C hw:0,0 -r 44100 -m 64 -M 64 -p -s 1
```

### Initial Test (Period Size = 64 Frames)

When called, the test/latency.c program will attemp to set period/buffer sizes based on the latency entered, starting from -m,--min option (or the default minimum latency = 64 if not specified). If the run succeeds without errors with that setting, the program exits; otherwise, the latency is increased, and the run repeated - if the run is succesful here, then program exits, else the process continues until the -M,--max latency is reached.

Example of successful run on BeagleY-AI with seeed2micvoicec soundcard:
```
Scheduler set to Round Robin with priority 99...
Playback device is hw:0,0
Capture device is hw:0,0
Parameters are 44100Hz, S16_LE, 2 channels, non-blocking mode
Poll mode: yes
Loop limit is 44100 frames, minimum latency = 64, maximum latency = 64
Hardware PCM card 0 'seeed2micvoicec' device 0 subdevice 0
Its setup is:
  stream       : PLAYBACK
  access       : RW_INTERLEAVED
  format       : S16_LE
  subformat    : STD
  channels     : 2
  rate         : 44100
  exact rate   : 44100 (44100/1)
  msbits       : 16
  buffer_size  : 64
  period_size  : 32
  period_time  : 725
  tstamp_mode  : NONE
  tstamp_type  : MONOTONIC
  period_step  : 1
  avail_min    : 32
  period_event : 0
  start_threshold  : 2147483647
  stop_threshold   : 64
  silence_threshold: 0
  silence_size : 0
  boundary     : 4611686018427387904
  appl_ptr     : 0
  hw_ptr       : 0
Hardware PCM card 0 'seeed2micvoicec' device 0 subdevice 0
Its setup is:
  stream       : CAPTURE
  access       : RW_INTERLEAVED
  format       : S16_LE
  subformat    : STD
  channels     : 2
  rate         : 44100
  exact rate   : 44100 (44100/1)
  msbits       : 16
  buffer_size  : 64
  period_size  : 32
  period_time  : 725
  tstamp_mode  : NONE
  tstamp_type  : MONOTONIC
  period_step  : 1
  avail_min    : 32
  period_event : 0
  start_threshold  : 2147483647
  stop_threshold   : 64
  silence_threshold: 0
  silence_size : 0
  boundary     : 4611686018427387904
  appl_ptr     : 0
  hw_ptr       : 0
Trying latency 64 frames, 1451.247us, 1.451247ms (689.0625Hz)
Success
Playback:
*** frames = 44192 ***
  state       : RUNNING
  trigger_time: 8580.707455
  tstamp      : 0.000000
  delay       : 48
  avail       : 16
  avail_max   : 48
Capture:
*** frames = 44128 ***
  state       : RUNNING
  trigger_time: 8580.707458
  tstamp      : 0.000000
  delay       : 0
  avail       : 0
  avail_max   : 32
Maximum read: 32 frames
Maximum read latency: 725.624us, 0.725624ms (1378.1250Hz)
Playback time = 8580.707455, Record time = 8580.707458, diff = -3
```

## Conclusion

Benchmarking ALSA on a real‑time kernel reveals critical insights into system behavior under low‑latency constraints. While our initial test at 1.45 ms round‑trip failed, the data guides adjustments—scheduler tuning, CPU isolation, and IRQ affinity—that can push the system toward sub‑millisecond performance. Future work will explore these optimizations and extend testing across different hardware codecs and sample rates.

---