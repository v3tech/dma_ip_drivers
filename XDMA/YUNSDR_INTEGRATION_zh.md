# YunSDR XDMA 驱动合并说明

本文记录 YunSDR XDMA 驱动合入 `v3tech/dma_ip_drivers` 的来源、目录映射、功能、编译安装方式及验证边界。

## 来源和合并方式

| 项目 | 内容 |
| --- | --- |
| 目标仓库 | [`v3tech/dma_ip_drivers`](https://github.com/v3tech/dma_ip_drivers) |
| 目标基线 | `master`，合并前提交 `9cdc9e335d51dc4e0e9d9fd6b4592ae55e30de72` |
| YunSDR 来源 | [`lichen813-gif/XDMA-Driver-YunSDR`](https://github.com/lichen813-gif/XDMA-Driver-YunSDR) |
| 来源提交 | [`529bc37`](https://github.com/lichen813-gif/XDMA-Driver-YunSDR/commit/529bc37) |
| 来源目录 | `linux-kernel/` |
| 合入目录 | `XDMA/linux-kernel/` |

两个目录具有相同的 XDMA 基线，且目标目录没有来源目录中缺失的独有文件。因此本次采用原位合并：把 YunSDR 修改放入目标仓库原有的 `XDMA/linux-kernel`，没有在仓库中再复制一套独立驱动，也没有修改 `QDMA`、`XVSEC` 和 `docs` 的现有内容。

## 合入内容

本次原位合并包含 14 个已有文件的修改和 6 个新增文件。主要变化如下：

- `xdma/cdev_ctrl.c`、`cdev_ctrl.h`：增加驱动拥有的 coherent DMA ring 分配、用户态 `mmap`、所有权检查、引用计数、release fence、可复用内存池、隔离回收及硬件 quiesce 后恢复；
- `xdma/libxdma_api.h`：增加 coherent ring/IOVA 用户态 ioctl ABI；`include/libxdma_api.h` 保留公共接口副本；
- `xdma/xdma_cdev.c`、`xdma_cdev.h`：在文件关闭和字符设备接口销毁时清理 DMA ring 所有权及可复用对象；
- `xdma/libxdma.c`、`libxdma.h`：默认启用轮询模式，补充 DMA mask、IRQ/关闭路径处理，并将 YunSDR 高速链路的 endpoint MRRS 设置为 1024 bytes；
- `xdma/cdev_sgdma.c`、`cdev_sgdma.h`：补充 Linux 6.x、RHEL/AlmaLinux 9.4 AIO 接口兼容和 NUMA 查询 ioctl；
- `xdma/xdma_thread.c`、`xdma_thread.h`、`xdma_mod.c`：调整轮询线程加锁、CPU/NUMA 信息和设备诊断输出；
- `xdma/Makefile`：支持 `KVERSION=<version>` 指定已安装内核的构建目录；
- 新增 `tests/fast_read_timestamp.c`、`tools/get_numa_node.c` 和历史 DKMS 辅助文件。

传统 XDMA read/write、测试程序和工具目录继续保留。coherent ring ABI 是 YunSDR 的可选扩展，只有匹配的 libyunsdr 和 PL/PS 能力协商通过后才应启用。

## 平台范围

同一份源码支持：

| 平台 | 架构 | 说明 |
| --- | --- | --- |
| NVIDIA Jetson / Linux ARM64 | `aarch64`、`arm64` | 用于 SMMU/IOVA coherent DMA ring 数据路径 |
| PC/服务器 Linux | `x86_64`、`amd64` | 保留传统接口并可使用新的 ring ABI |

这里的 ARM 支持仅指 64 位 AArch64，不声明支持 32 位 ARM。内核模块必须针对目标机器当前运行的内核编译；不同架构、内核版本或内核配置生成的 `xdma.ko` 不能混用。

## 编译

Debian/Ubuntu 安装依赖：

```bash
sudo apt update
sudo apt install -y build-essential linux-headers-"$(uname -r)" pciutils
test -f "/lib/modules/$(uname -r)/build/Makefile"
```

ARM64 与 x86_64 使用相同命令：

```bash
cd XDMA/linux-kernel/xdma
make clean
make -j"$(nproc)"
modinfo ./xdma.ko | grep -E 'filename|version|vermagic|srcversion'
```

为已安装但未运行的内核编译时：

```bash
make clean
make KVERSION=<kernel-version> -j"$(nproc)"
```

`/lib/modules/<kernel-version>/build` 必须存在。启用 Secure Boot 时，还需要按发行版要求给模块签名。

## 安装和加载

先停止所有正在访问 YunSDR/XDMA 设备的进程，再执行：

```bash
cd XDMA/linux-kernel/xdma
sudo modprobe -r xdma 2>/dev/null || true
sudo install -D -m 0644 xdma.ko \
  "/lib/modules/$(uname -r)/updates/dkms/xdma.ko"
sudo depmod -a
sudo modprobe xdma
```

验证模块和设备：

```bash
modinfo xdma | grep -E 'filename|version|vermagic|srcversion'
ls -l /dev/xdma*
lspci -nn | grep -i xilinx
sudo dmesg | tail -n 100
```

驱动默认 `poll_mode=1`。需要明确指定时可使用：

```bash
sudo modprobe xdma poll_mode=1
```

YunSDR IQX8400 高速链路使用 1024-byte MRRS。加载后应通过 `lspci -vv -s <BDF>` 核对链路速率、宽度和 `MaxReadReq`。

## 配套版本和 ABI 边界

coherent ring/IOVA 数据路径需与以下配套项目的兼容版本共同使用：

- [`lichen813-gif/libyunsdr-src`](https://github.com/lichen813-gif/libyunsdr-src)；
- [`lichen813-gif/iqx8400-pcies-split`](https://github.com/lichen813-gif/iqx8400-pcies-split)。

旧 PL/PS 或旧 libyunsdr 不应强制启用新的 ring ABI。能力、ring 所有者、子卡、方向和 release fence 任一不匹配时，应中止 IOVA 启动，而不是继续使用可能仍被硬件访问的地址。

## 验证边界

来源提交报告的目标构建环境为：

- Jetson AArch64：Linux `6.8.12-1021-tegra`；
- x86_64：Linux `6.8.0-138-generic`。

来源版本曾在上述两类目标内核完成编译并用于 IQX8400 IOVA 测试。本次合并只改变仓库中的目录位置，没有改变驱动源码内容；本次提交未重新执行目标机器编译或硬件回归。

编译成功只证明源码与目标内核接口兼容。正式发布仍需在目标机器检查 PCIe link、SMMU/IOMMU fault、DMA 长时间双向传输、进程异常退出、模块卸载、ring fence/回收以及配套 PL/PS 和 libyunsdr 的协议一致性。

## 许可证

合入文件继续遵循其文件头、`XDMA/linux-kernel/LICENSE` 和 `XDMA/linux-kernel/COPYING` 所声明的许可证。目标仓库其他组件的许可证和版权声明不因本次合并而改变。
