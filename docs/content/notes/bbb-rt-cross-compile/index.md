+++
title = "Building and Installing a Real-Time Kernel on BeagleBone Black"
description = ""
date = 2025-05-25
[taxonomies]
tags = ["Beaglebone Black", "Linux Kernel", "Preempt-RT"]
+++

#### **Introduction**  
![BeagleBone® Black](https://www.beagleboard.org/app/uploads/2023/07/DSC00505-400x345.webp)  
The BeagleBone Black (BBB) is a versatile embedded platform, but its stock Linux kernel isn’t optimized for **hard real-time tasks** (e.g., robotics, industrial control). By compiling and installing a **Preempt-RT** (Real-Time) patched kernel, you can achieve microsecond-level latency guarantees. This guide walks through cross-compiling and deploying the `6.6.87-bone-rt-r27` kernel on Debian 12.10.

---

#### **Why a Real-Time Kernel?**  
- **Preempt-RT Patch**: Converts Linux into a hard real-time OS by minimizing non-interruptible code paths.  
- **Use Cases**: Motor control, sensor fusion, high-speed data acquisition.  
- **Latency**: Reduces worst-case latency from milliseconds to **~50μs** on the BBB.  

---

### **Prerequisites**  
1. **Hardware**:  
   - BeagleBone Black (Rev C recommended).
   - MicroSD card (8GB+).
   - USB cable (for power/data).
2. **Software**:  
   - **Base Image**: [Debian 12.10 (2025-05-14)](https://www.beagleboard.org/distros/beaglebone-black-debian-12-10-2025-05-14-iot-vscode-v6-12-x).  
   - **Host Machine**: Ubuntu 22.04 (or any Linux distro with ARM cross-compiler).  

---

### **Step 1: Prepare the BBB**  
1. Flash the Debian image to the SD card: 
   - [bb-imager-rs](https://github.com/beagleboard/bb-imager-rs) (recomended)
   - [balena Etcher](https://etcher.balena.io/) (alternative)
2. Boot the BBB from the SD card.  

---

### **Step 2: Cross-Compile the Kernel**  
**On your host machine**:  
1. Clone Robert Nelson’s build scripts (official BBB kernel maintainer):  
   ```bash  
   git clone https://github.com/RobertCNelson/bb-kernel.git kernelbuildscripts  
   cd kernelbuildscripts  
   ```  
2. Checkout the real-time kernel branch:  
   ```bash  
   git checkout 6.6.87-bone-rt-r27 -b rt-kernel  
   ```  
3. Start the build (takes 20-60 minutes):  
   ```bash  
   ./build_kernel.sh  
   ```

4. Generated Artifacts (in `./deploy/`):  
   - `6.6.87-bone-rt-r27.zImage` (compressed kernel).  
   - `6.6.87-bone-rt-r27-dtbs.tar.gz` (device trees).  
   - `6.6.87-bone-rt-r27-modules.tar.gz` (kernel modules).  

---

### **Step 3: Install the Kernel on the BBB**  
1. **Connect to the BBB** via USB:  
   ```bash  
   ssh debian@192.168.7.2  # Password: temppwd  
   ```  
   *(For macOS, use `192.168.6.2`)*.  

2. **Transfer Files** from host to BBB:  
   ```bash  
   scp deploy/6.6.87-bone-rt-r27-* debian@192.168.7.2:/home/debian  
   ```  

3. **On the BBB**:  
   - Install kernel modules:  
     ```bash  
     # 1. Create temporary extraction directory
     mkdir ~/kernel_temp
     # 2. Extract modules to temporary location
     tar xvf 6.6.87-bone-rt-r27-modules.tar.gz -C ~/kernel_temp
     # 4. Safely move modules to /lib/modules
     sudo mv ~/kernel_temp/lib/modules/6.6.87-bone-rt-r27 /lib/modules/
     # 5. Cleanup temporary files
     rm -rf ~/kernel_temp
     ```  
   - Install the kernel image:  
     ```bash  
     sudo cp 6.6.87-bone-rt-r27.zImage /boot/vmlinuz-6.6.87-bone-rt-r27  
     ```  
   - Install device trees:  
     ```bash  
     sudo tar xvf 6.6.87-bone-rt-r27-dtbs.tar.gz -C /boot/dtbs/  
     ```  
   - Copy kernel config:  
     ```bash  
     sudo cp config-6.6.87-bone-rt-r27 /boot/  
     ```  

4. **Generate initramfs**:  
   ```bash  
   sudo update-initramfs -uk 6.6.87-bone-rt-r27  
   ```  

5. Update U-Boot to load the new kernel:  
   ```bash  
   sudo nano /boot/uEnv.txt  
   ```  
   Add/modify:  
   ```ini  
   uname_r=6.6.87-bone-rt-r27  
   ```  

---

### **Step 4: Reboot and Verify**  
1. Reboot the BBB:  
   ```bash  
   sudo reboot  
   ```  
2. Confirm the real-time kernel is active:  
   ```bash  
   uname -r  # Should show "6.6.87-bone-rt-r27"  
   ```  
3. Test real-time capabilities:  
   ```bash  
   sudo cyclictest -t1 -p80 -n -i 10000 -l 10000  
   ```

---

### **Troubleshooting**  
- **Boot Failure**: Revert by selecting the old kernel in `uEnv.txt`.  
- **Missing Modules**: Ensure modules are in `/lib/modules/6.6.87-bone-rt-r27/`.  
- **High Latency**: Disable CPU frequency scaling:  
  ```bash  
  sudo cpufreq-set -g performance  
  ```  

---

### **Conclusion**  
You now have a real-time kernel running on your BeagleBone Black! This unlocks deterministic performance for time-sensitive applications. For production systems, consider:  
- Tuning the kernel via `/sys/kernel/realtime`.  
- Using hardware-assisted PWM/PRU cores for nanosecond precision.  

---

### **References**  
1. [Official bb-kernel Build Scripts](https://github.com/RobertCNelson/bb-kernel)  
2. [BeagleBone Real-Time Kernel Guide](https://hackmd.io/@ericlinsechs/How-to-Build-Linux-kernel-for-BeagleBone-Black)  
3. [Preempt-RT Patch Documentation](https://wiki.linuxfoundation.org/realtime/documentation/start)  