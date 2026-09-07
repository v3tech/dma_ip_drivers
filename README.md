# Xilinx DMA IP Reference drivers

## YunSDR XDMA extension

The XDMA Linux driver under `XDMA/linux-kernel` includes the YunSDR ARM64/x86_64
extension merged from
[`lichen813-gif/XDMA-Driver-YunSDR`](https://github.com/lichen813-gif/XDMA-Driver-YunSDR).
It adds the coherent DMA ring/IOVA userspace ABI, release fencing and recovery,
newer-kernel compatibility, and the YunSDR polling/MRRS configuration while
retaining the traditional XDMA interfaces.

Build, installation, source provenance, compatibility requirements, and the
exact integration scope are documented in
[`XDMA/YUNSDR_INTEGRATION_zh.md`](XDMA/YUNSDR_INTEGRATION_zh.md).

## Xilinx QDMA

The Xilinx PCI Express Multi Queue DMA (QDMA) IP provides high-performance direct memory access (DMA) via PCI Express. The PCIe QDMA can be implemented in UltraScale+ devices.

Both the linux kernel driver and the DPDK driver can be run on a PCI Express root port host PC to interact with the QDMA endpoint IP via PCI Express.

### Getting Started

* [QDMA Reference Drivers Comprehensive documentation](https://xilinx.github.io/dma_ip_drivers/)

## Xilinx-VSEC (XVSEC)

Xilinx-VSEC (XVSEC) are Xilinx supported VSECs. The XVSEC Driver helps creating and deploying designs that may include the Xilinx VSEC PCIe Features.

VSEC (Vendor Specific Extended Capability) is a feature of PCIe.

The VSEC itself is implemented in the PCIe extended capability register in the FPGA hardware (as either soft or hard IP). The drivers and SW are created to interface with and use this hardware implemented feature.

The XVSEC driver currently include the MCAP VSEC, but will be expanded to include the XVC VSEC and NULL VSEC.

### Getting Started

* [XVSEC Linux Kernel Reference Driver Comprehensive documentation](https://xilinx.github.io/dma_ip_drivers/)

### Support

Refer to Xilinx PCIe Forum for any queries/issues/support required w.r.t Xilinx's DMA IP Reference Drivers

Note: Issues are disabled in github for these drivers. All the queries shall be redirected through Xilinx PCIe Forum link given below.

* [Xilinx PCIe Forum](https://forums.xilinx.com/t5/PCIe-and-CPM/bd-p/PCIe)
