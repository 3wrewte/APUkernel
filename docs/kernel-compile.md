# 内核编译与 PXE 引导踩坑记录

本文档记录为 A8-7500 交叉编译内核并配置 PXE 网络引导过程中遇到的所有问题和解决方案。

## 环境

- **构建机**: openSUSE Leap 16.0, x86_64
- **目标机**: AMD A8-7500 (Kaveri APU), Debian 13 trixie
- **内核源码**: 位于 `kernel-deb/` (基于 Debian 6.12.86 配置)

---

## 1. dracut initramfs 跨机器构建失败

### 问题

在 openSUSE 构建机上为 A8-7500 构建 initramfs 时，dracut 存在以下问题：

1. **`--kmoddir` 不完整生效**: 虽然通过 `--kmoddir /tmp/kmod-staging/lib/modules/6.12.86` 指定了模块路径，但 dracut 内部仍会 `realpath /lib/modules/6.12.86` 检查默认路径是否存在，若不存在则报错：
   ```
   realpath: /lib/modules/6.12.86: No such file or directory
   grep: /lib/modules/6.12.86/modules.builtin: No such file or directory
   ```

2. **hostonly 污染**: 即使加了 `--no-hostonly`，dracut 仍会从构建机的 `/etc` 读取配置（如 `/etc/iscsi/iscsid.conf`），在环境差异较大时直接失败：
   ```
   cp: cannot open '/etc/iscsi/iscsid.conf' for reading: Permission denied
   dracut[E]: FAILED: /usr/lib/dracut/dracut-install ... /etc/iscsi/iscsid.conf
   ```

3. **硬编码 UUID**: 若 `--no-hostonly` 实际未生效（比如因上述错误提前退出），构建出的 initramfs 会硬编码构建机的 root UUID，导致目标机挂载失败：
   ```
   dracut-initqueue: timeout, still waiting for ... /dev/disk/by-uuid/<构建机UUID>
   ```

### 教训

**dracut 不适合跨机器/跨发行版构建 initramfs。** 它深度绑定构建机的文件系统布局、发行版配置和硬件特征。以下方案更可靠：

- 编译内核时把所有引导所需驱动写成 built-in (`=y`)，完全省略 initramfs
- 或在目标机上直接构建 initramfs
- 或使用更可移植的工具（如 mkinitcpio）或手工构建最小 initramfs

### 相关配置

内核中必须 built-in 的驱动链：

```
CONFIG_SCSI=y          # SCSI 子系统
CONFIG_ATA=y           # ATA 总线
CONFIG_SATA_AHCI=y     # SATA 控制器
CONFIG_BLK_DEV_SD=y    # 磁盘设备
CONFIG_EXT4_FS=y       # ext4 文件系统
```

---

## 2. CONFIG_DRM 依赖链回退

### 问题

手动将 `CONFIG_DRM_AMDGPU=m` 改为 `=y` 后，`make olddefconfig` 会自动改回 `=m`。

原因：`CONFIG_DRM_AMDGPU` 依赖 `CONFIG_DRM_TTM`（TTM 显存管理器），而 `CONFIG_DRM_TTM` 又依赖 `CONFIG_DRM`。若 `CONFIG_DRM=m`，整个依赖链上的所有选项最多只能是 `=m`，`olddefconfig` 会强制修正不一致的配置。

### 解决方案

**必须从依赖链顶端开始改起。** 按顺序逐层改为 `=y`：

```
CONFIG_DRM=y           # 先改这个 (root dependency)
CONFIG_DRM_TTM=y       # 再改 TTM
CONFIG_DRM_SCHED=y     # DRM scheduler
CONFIG_DRM_BUDDY=y     # DRM buddy allocator
CONFIG_DRM_AMDGPU=y    # 最后改 amdgpu
```

每次改完后运行 `make olddefconfig` 确认未被回退。

### 依赖链图示

```
CONFIG_DRM (=y)
 ├── CONFIG_DRM_TTM (=y)
 │    └── CONFIG_DRM_AMDGPU (=y)
 │         └── CONFIG_HSA_AMD (=y, KFD)
 ├── CONFIG_DRM_SCHED (=y)
 └── CONFIG_DRM_BUDDY (=y)
```

---

## 3. `make olddefconfig` 自动修正

### 问题

手动编辑 `.config` 后直接编译，可能导致配置不一致或编译失败。

### 正确做法

**每次修改 `.config` 后，必须运行 `make olddefconfig` 解决依赖关系**：

```bash
# 1. 编辑 .config
vim kernel-deb/.config

# 2. 自动解决依赖（非交互式）
make -C kernel-deb olddefconfig

# 3. 确认修改未被回退
grep '期望的选项' kernel-deb/.config

# 4. 重新编译
make -C kernel-deb -j$(nproc)
```

`olddefconfig` 的行为：
- 对新出现的配置项使用默认值
- 修正不符合 `select`/`depends` 约束的选项
- 会静默回退不一致的手动修改

---

## 4. 内置驱动无法加载固件

### 问题

amdgpu 编译为 built-in (`=y`) 后，驱动在 PCI 总线枚举阶段初始化，此时尝试通过 `request_firmware()` 加载 `amdgpu/kaveri_*.bin`，但报错：

```
amdgpu 0000:00:01.0: Direct firmware load for amdgpu/kaveri_sdma.bin failed with error -2
[drm:amdgpu_device_init.cold] *ERROR* early_init of IP block <cik_sdma> failed -19
amdgpu 0000:00:01.0: amdgpu: Fatal error during GPU init
```

**错误码 -2 = ENOENT (No such file or directory)**。

### 排查过程

1. **先怀疑固件文件不存在** → 确认 `/lib/firmware/amdgpu/kaveri_sdma.bin` 存在（Debian 固件包已安装）
2. **再怀疑符号链接问题** → Debian 固件包中部分 kaveri 固件是符号链接（如 `kaveri_sdma.bin -> bonaire_sdma.bin`），替换为实际文件 → **仍失败**
3. **最终确认根因**: built-in 驱动在 PCI 枚举阶段就尝试加载固件，此时 rootfs 虽然已挂载但文件系统 IO 路径可能尚未完全就绪；或者直接固件加载（`FW_LOADER_USER_HELPER_FALLBACK` 未启用，不经过 udev）在早期启动阶段不可靠

### 解决方案

**将固件嵌入内核镜像**（CONFIG_EXTRA_FIRMWARE）：

```bash
# 1. 准备固件目录（从 openSUSE 的 .xz 压缩固件解压）
mkdir -p /tmp/kmod-staging/firmware/amdgpu
for f in /lib/firmware/amdgpu/kaveri_*.bin.xz; do
    xz -d < "$f" > "/tmp/kmod-staging/firmware/amdgpu/$(basename "$f" .xz)"
done

# 2. 修改 .config
CONFIG_EXTRA_FIRMWARE="amdgpu/kaveri_ce.bin amdgpu/kaveri_me.bin amdgpu/kaveri_mec.bin amdgpu/kaveri_mec2.bin amdgpu/kaveri_pfp.bin amdgpu/kaveri_rlc.bin amdgpu/kaveri_sdma.bin amdgpu/kaveri_sdma1.bin amdgpu/kaveri_uvd.bin amdgpu/kaveri_vce.bin"
CONFIG_EXTRA_FIRMWARE_DIR="/tmp/kmod-staging/firmware"

# 3. 重新编译
make -C kernel-deb olddefconfig
make -C kernel-deb -j$(nproc)
```

**优点**: 固件编译进内核镜像，不依赖 rootfs 的任何文件，在最早启动阶段即可用。
**缺点**: vmlinuz 增大 ~400KB，每次更换固件需重编译。

### 如何确定需要哪些固件

1. 先在目标机上正常启动（如用原 Debian 内核）
2. `dmesg | grep "firmware.*loaded"` 查看已加载的固件列表
3. 或直接搜驱动源码中的固件文件名（`grep -r kaveri drivers/gpu/drm/amd/`）

---

## 5. amdgpu 不绑定 CIK GPU

### 问题

amdgpu 已 built-in 且无固件报错，但仍不绑定 Kaveri GPU (1002:1313)。

### 根因

Kaveri 属于 Sea Islands / CIK 架构。从 Linux 4.x 起，amdgpu 对 CIK GPU 支持默认禁用，必须同时满足两个条件：

1. **内核配置**: `CONFIG_DRM_AMDGPU_CIK=y`
2. **内核命令行**: `amdgpu.cik_support=1`

缺少任一条件，amdgpu 都不会探测 CIK GPU 设备。

### 检查方法

```bash
grep CONFIG_DRM_AMDGPU_CIK .config    # 应为 =y
cat /proc/cmdline                       # 应有 amdgpu.cik_support=1
```

---

## 6. 其他注意事项

### `/lib` → `/usr/lib` symlink (usrmerge)

Debian 13 使用 usrmerge，`/lib` 是 `/usr/lib` 的符号链接。内核的固件加载器能正常追踪符号链接，但如果固件文件本身是符号链接（如 `kaveri_sdma.bin -> bonaire_sdma.bin`），早期启动阶段可能无法解析。**建议使用实际文件而非符号链接。**

### 模块安装

PXE 场景下，自定义内核模块需要手动安装到目标机 rootfs：

```bash
# 在目标机上
tar xzf kmod-6.12.86.tar.gz -C /lib/modules/
depmod 6.12.86
```

注意：`depmod` 需要 `kmod` 工具包（`apt install kmod`）。

### PXE 引导链

```
A8-7500 (UEFI PXE)
  → DHCP (dnsmasq on enp193s0)
    → TFTP boot/grub2/x86_64-efi/core.efi (GRUB2)
      → GRUB 读取 grub.cfg (TFTP)
        → HTTP GET http://192.168.2.1/vmlinuz
          → 内核直接挂载 /dev/sda2 启动
```

无 initramfs，GRUB 只用 `linux` 命令加载内核，不用 `initrd`。

### grub.cfg 路径 (grub2-mknetdir)

**关键坑**: `grub2-mknetdir --net-directory=/srv/tftp` 在 openSUSE 上的默认 subdir 与 GRUB 内嵌的 `$prefix` 路径可能不匹配。

- GRUB 内嵌的 `$prefix` = `(tftp,192.168.2.1)/boot/grub2`
- grub.cfg 查找路径 = `$prefix/grub.cfg` = `/srv/tftp/boot/grub2/grub.cfg`
- grub2-mknetdir 实际输出 = `/srv/tftp/boot/grub2/x86_64-efi/grub.cfg`

**修复**: 在 `$prefix/grub.cfg` 放一个 mini chain-loader：
```
configfile $prefix/x86_64-efi/grub.cfg
```

或部署时直接 `cp` 到正确位置。

### TFTP blocksize 兼容性

dnsmasq 默认启用 TFTP blocksize 协商（最大 1468 字节），但 GRUB2 的 `tftp.mod` 对此处理不佳，导致 `ls` 无输出、`configfile` 静默失败。

**修复**: dnsmasq.conf 添加 `tftp-no-blocksize`，强制使用标准 512 字节块。

### dnsmasq 的 `bind-interfaces`

使用 `bind-interfaces` + `interface=enp193s0` 确保 DHCP 和 TFTP 服务**仅**绑定在 PXE 网段接口，不影响家庭网络（enp194s0）。配合 `port=0` 禁用 DNS 功能。

---

## 编译命令速查

```bash
# 修改配置
vim kernel-deb/.config
make -C kernel-deb olddefconfig

# 全量/增量编译
make -C kernel-deb -j$(nproc)

# 安装模块到暂存区
sudo make -C kernel-deb modules_install INSTALL_MOD_PATH=/tmp/kmod-staging

# 安装模块到本机 (一般不推荐，会污染 build host)
# sudo make -C kernel-deb modules_install

# 部署内核到 HTTP root
cp kernel-deb/arch/x86/boot/bzImage /srv/http/vmlinuz

# 部署 grub.cfg 到 TFTP
sudo cp pxe/grub.cfg /srv/tftp/boot/grub2/x86_64-efi/grub.cfg

# PXE 服务管理
sudo ./scripts/pxe/start.sh
sudo ./scripts/pxe/stop.sh
./scripts/pxe/status.sh
```
