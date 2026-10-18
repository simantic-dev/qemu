NXP i.MX 93 11x11 Evaluation Kit (``imx93-11x11-evk``)
======================================================

The ``imx93-11x11-evk`` machine models the NXP i.MX 93 11x11 LPDDR4X
Evaluation Kit. The i.MX 93 is a dual Cortex-A55 applications processor
with a Cortex-M33 real-time core. Unlike the i.MX 95, it has no System
Manager: Linux programs the clock (CCM), analog PLLs (ANATOP), reset and
power-domain blocks directly, so those are modelled functionally rather
than served by firmware over SCMI.

Supported devices
-----------------

The ``imx93-11x11-evk`` machine implements the following devices:

 * 2 Cortex-A55 application cores
 * 1 Cortex-M33 real-time core (heterogeneous, with private ITCM/DTCM)
 * Generic Interrupt Controller (GICv3)
 * LPUART serial controllers (LPUART1 is the Linux console)
 * CCM clock controller and ANATOP PLLs
 * MEDIAMIX block control / GPR and SRC power-domain slice
 * LPI2C controllers with the board PMIC (PCA9451A), the PCAL6524,
   ADP5585 and PCA9538 I/O expanders, and an MT9M114 camera sensor
 * GPIO controllers
 * ELE (EdgeLock Enclave) messaging-unit responder
 * MU1 Messaging Unit (A55 <-> M33 mailbox)
 * eDMA3 and eDMA4 controllers
 * uSDHC (SD/MMC)
 * FEC and eQOS (dwmac4) Ethernet
 * ChipIdea USB host controllers
 * Display: LCDIFv3 controller with a MIPI-DSI host + ADV7535 HDMI bridge,
   and an LDB + LVDS-PHY path to a fixed LVDS panel
 * Audio: SAI (I2S) and MICFIL (PDM) front-ends with a WM8962 codec
 * Camera: MT9M114 sensor -> parallel-CSI -> ISI V4L2 capture pipeline
 * Ethos-U65 microNPU register block (driven by the M33 firmware)
 * virtio-mmio transports (for virtio-keyboard / -tablet input)
 * FlexCAN controllers on QEMU's CAN bus

Other peripherals are instantiated as unimplemented-device stubs that log
accesses.

Boot options
------------

The machine boots a Linux kernel image with ``-kernel`` plus a device
tree (``-dtb``) and a root filesystem; these are built from the NXP i.MX
BSP and are not shipped with QEMU. The base device tree drives the HDMI
display path; the ``...-boe-wxga-lvds-panel`` device tree selects the
LVDS panel path instead.

.. code-block:: bash

  $ qemu-system-aarch64 -M imx93-11x11-evk -m 4G -display none \
      -kernel Image -dtb imx93-11x11-evk.dtb -initrd rootfs.cpio.gz \
      -append "earlycon=lpuart32,mmio32,0x44380010 \
               console=ttyLP0,115200 cpuidle.off=1 rdinit=/init" \
      -serial mon:stdio -serial null

The earlycon address is the LPUART base plus the driver's ``0x10``
register offset (``0x44380010``), which earlycon, unlike the regular
driver, does not apply itself.

CAN bus
-------

Attach a CAN bus to the FlexCAN controllers with, for example::

  -object can-bus,id=cb -machine canbus0=cb,canbus1=cb

Input
-----

A ``virtio-keyboard-device`` (and optionally ``virtio-tablet-device``)
binds to the modelled virtio-mmio transports, delivering keyboard input
to the framebuffer console when run with a graphical display backend.

Cortex-M33 real-time core
-------------------------

The Cortex-M33 is instantiated as an additional, always-present core with
its own ITCM/DTCM. It is held in reset until firmware is staged into its
ITCM, so a plain Linux boot is unaffected. Stage a firmware image (a raw
``.bin`` linked for the TCM, as produced from the NXP M-core SDK) at the
A55-side ITCM alias and let the machine release the core::

  -device loader,file=m33_firmware.bin,addr=0x201e0000,force-raw=on

With NXP's ``rpmsg_lite`` firmware on the M33 and ``imx_rpmsg_pingpong``
on Linux, the M33 brings its RPMsg link up over MU1, announces a channel,
and messages round-trip through the shared vrings. For remoteproc to
attach, the firmware's resource table must also be present at the
``rsc-table`` reserved-memory region (as U-Boot's ``bootaux`` would place
it); stage it with a second ``-device loader,...,addr=0x2021e000``.

Alternatively Linux can boot the M33 itself, on demand, through
remoteproc: the i.MX rproc driver issues the i.MX SiP ``RPROC`` SMC,
which the machine services (releasing the M33 at its staged vector). This
is how the Ethos-U65 microNPU comes up - it has no Linux-visible
registers and is driven by firmware on the M33. Opening ``/dev/ethosu0``
makes the ethosu driver load the NXP ethos firmware, boot the M33, and
connect over ``rpmsg-ethosu-channel``.

Ethos-U65 microNPU
------------------

The NPU register block (base ``0x4a900000``) is modelled enough for the
M33 ethos firmware to identify, reset and configure the part. Its
completion interrupt is wired to the M33 NVIC (IRQ 178). The
command-stream compute engine is not modelled; a real inference is not
executed inside QEMU. (The fork's ``tests/ethosu-infer`` demonstrates a
correct end-to-end inference by running a reference TFLite model on the
host when the firmware kicks the NPU, but that host stand-in is a
demo-only path and is not part of the upstream machine.)
