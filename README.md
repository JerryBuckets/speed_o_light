# Project: PWM LEDs & Button‑Press Reader

This project provides:

- A **kernel module** (`project.ko`) that  
  - Drives three LEDs on GPIO 25, 6, and 17 via software PWM  
  - Exposes both a **character device** (`/dev/project`) and **sysfs entries**  
    (`/sys/class/project/project/led1_duty`, `led2_duty`, `led3_duty`)  
  - Tracks alternating button presses on GPIO 26 and GPIO 12 in a 10 s sliding window  

- A **Rust user‑space program** (`main`) that reads the 10 s press count from `/dev/project`, computes a percentage (capped at 100%), and either:  
  - Writes it back into `/dev/project` (`--dev` mode)  
  - Splits it across the three LEDs via sysfs (`--sys` mode)  

---

## Prerequisites

- **Raspberry Pi** with matching Linux kernel headers installed  
- **GCC** (for kernel module build)  
- **Rust toolchain** (`rustc`, optional `cargo`)  
- **make**  

---

## Quickstart & Commands

Run the following commands **in order**:

```bash
# 1. Build both the kernel module and Rust program
make all

# 2. Load the kernel module (creates /dev/project & sysfs entries)
sudo make load

# 3. (Optional) Grant full permissions so any user can read/write
sudo chmod 777 /dev/project
sudo chmod 777 /sys/class/project/project/led1_duty \
                  /sys/class/project/project/led2_duty \
                  /sys/class/project/project/led3_duty

# 4. Run the Rust reader in “dev” mode (writes back to /dev/project)
sudo ./build/main --dev

# 5. Or in “sys” mode (writes via sysfs to the three LED files)
sudo ./build/main --sys

# 6. To stop and unload:
#    - Ctrl+C your Rust program
sudo make unload

# 7. Clean up all build artifacts
make clean

