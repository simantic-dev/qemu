# SG2002 bring-up harness (Simantic QEMU fork)

Goal: let firmware and application teams work on Sophgo SG2002 boards before
hardware is available. This fork is a bring-up harness, not a product. The MMIO
traces, fixtures and acceptance tests it produces carry over to the RV64 work
in the pyrite simulator.

## Scope

- **Simulated:** the big T-Head C906 core (RV64GC, Sv39) running the vendor
  boot chain and Linux, the board device tree, sensor control over I2C, MIPI
  CSI-2 receive and VI capture into DDR, and the USB host with a UVC camera.
- **Not simulated:** ISP image processing (frames pass through), VENC
  bitstream generation (done by a host encoder behind the command interface),
  the TPU, RVV 0.7.1, the small C906 core, the Cortex-A53 and the 8051.

## What exists upstream

- `target/riscv`: the `thead-c906` CPU, with XThead* extensions but no MAEE
  and no RVV 0.7.1.
- `hw/usb/hcd-dwc2.c`: a DWC2 host controller.
- Under review: a Milk-V Duo (CV1800B) machine series
  (https://patchew.org/QEMU/20260616190147.1286316-1-visitorckw@gmail.com/).

## Steps

Each step has the check that counts as done.

1. **App sandbox.** `-M virt -cpu thead-c906`, a buildroot rootfs, and a CVI
   MPI shim that covers the VI/VPSS/VENC subset the app uses. The shim takes
   frames from a file or test pattern, encodes H.264 on the host, times
   delivery on a frame clock, and can inject faults (drop, stall, throttle).
   - Done when a sample app captures N frames and produces a decodable stream.
2. **SG2002 machine.** Start from the Duo series and remap it to
   `memorymap_sg2002` (sophgo-doc, SG200X TRM).
   - Done when a stock Duo 256M image boots headless from FSBL through OpenSBI
     and U-Boot to a Linux shell.
3. **Sensor I2C model.** Model chip-ID and the stream-on sequence.
   - Done when the sensor driver probes.
4. **MIPI-RX/CIF and VI.** Built from the TRM `mipi_rx_*` and `vi_*` register
   tables and the SDK `osdrv/interdrv/v2/{cif,vi}` sources.
   - Done when the SDK VI sample captures N frames that match the injected
     file.
5. **USB.** Wire up the DWC2 host and add a UVC device that serves frames
   from a file.
   - Done when `uvcvideo` enumerates the device and streams.
6. **Silicon traces.** Capture MMIO traces of vendor VI, ISP and VPSS init on
   a Duo 256M or a LicheeRV Nano, and record the silicon and core revision.
   - Done when the traces replay against the step 4 model.

## References

- TRM: https://github.com/sophgo/sophgo-doc (SG200X)
- SDK: https://github.com/milkv-duo/duo-buildroot-sdk-v2
- Mainline device tree: `arch/riscv/boot/dts/sophgo/sg2002.dtsi`

## Provenance

QEMU's `AGENTS.md` and `docs/devel/code-provenance.rst` forbid AI-generated
content in upstream contributions. All agent-written code on this fork is local
to Simantic and must never be submitted upstream.
