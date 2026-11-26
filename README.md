# rknpu356x-module

Out-of-tree kernel module for the Rockchip NPU (`fde40000.npu`, RK3566/RK3568) on Linux 7.3.x, paired with a custom userspace runtime in Rust (`0 unsafe`) designed to bypass the ~2 GB memory cap and closed behavior of `librknnrt.so`.

## 1. Tested Environment

| Item | Value |
|---|---|
| Board | BigTreeTech Pi2 v1.0.1 (RK3566, 8 GB RAM) |
| OS | Armbian (Debian Trixie) |
| Kernel | `7.3.4-silex-rockchip64` |
| Reference Userspace | `librknnrt v2.3.2` (baseline reference / being replaced) |
| Target Runtime | Custom Rust (`0 unsafe`) client over `/dev/dri/renderD*` |

## 2. Devfreq (DVFS)

The driver supports dynamic voltage and frequency scaling via the standard Linux devfreq framework. When an OPP table is present in the DTB, the driver automatically registers a devfreq device at `/sys/class/devfreq/fde40000.npu/`.

Available frequencies (RK3566): `200 / 297 / 400 / 600 / 700 / 800 MHz`.

```sh
# Check current frequency
cat /sys/class/devfreq/fde40000.npu/cur_freq

# List available frequencies
cat /sys/class/devfreq/fde40000.npu/available_frequencies

# Switch to manual frequency control (800 MHz)
echo userspace > /sys/class/devfreq/fde40000.npu/governor
echo 800000000 > /sys/class/devfreq/fde40000.npu/min_freq
echo 800000000 > /sys/class/devfreq/fde40000.npu/max_freq

# Restore automatic governor
echo rknpu_ondemand > /sys/class/devfreq/fde40000.npu/governor
echo 200000000 > /sys/class/devfreq/fde40000.npu/min_freq
echo 800000000 > /sys/class/devfreq/fde40000.npu/max_freq
```

## 3. What Has Been Done

- **DTB-driven probe & bring-up:** Verified clean module load (`rknpu`), IRQ assignment, DRM render node registration, and `devfreq` startup on `7.3.4-silex-rockchip64`.
- **Memory & architecture evaluation:** Analyzed 8 GB RAM behavior, CMA vs. IOMMU scatter-gather mappings, the NPU's 32-bit hardware addressing limit, and the ~2 GB constraint observed in `librknnrt.so`.
- **Job timeout reproduction & diagnosis:** Observed job timeouts (`task counter: 0`, `raw status: 0x10000`, `require mask: 0x300`, followed by soft reset) under proprietary userspace execution, motivating a clean, auditable stack.
- **Repository isolation & hardening plan:** Separated active/experimental work into a dedicated repository so the working DTB bring-up baseline remains untouched.

## 4. Current Work in Progress

- **Hardened C kernel driver (RK3566/RK3568):**
  - Scatter-gather (`sg_table`) buffer management mapped via IOMMU without depending on CMA for large tensor buffers (up to 4 GB buffer/range bounds, 32-bit IOVA checks for NPU descriptors).
  - Strict `ioctl` boundary validation (`check_add_overflow`, range checks, capability enforcement, no raw kernel pointers exposed to userspace, and single-owner job/fence cleanup).

