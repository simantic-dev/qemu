// SPDX-License-Identifier: GPL-2.0-only
/*
 * ethosu_caps - query the Ethos-U65 microNPU capabilities from Linux user space.
 *
 * This is a fork-only demo tool for the QEMU i.MX93 machine. It proves the full
 * A55 -> M33 -> NPU round-trip: opening /dev/ethosu0 makes the kernel ethosu
 * driver boot the M33 (on demand, via the i.MX SiP RPROC SMC the machine
 * services), the M33 firmware brings up rpmsg-ethosu-channel, and a
 * CAPABILITIES_REQ travels A55 -> (MU/rpmsg) -> M33 -> (reads the modelled NPU
 * ID/CONFIG registers) -> back. We print what the NPU reported.
 *
 * Build:  aarch64-linux-gnu-gcc -static -O2 -o ethosu_caps ethosu_caps.c
 *
 * It is NOT an upstream QEMU deliverable - it talks to a Linux char device, not
 * to QEMU. It lives here only to exercise/demonstrate the modelled NPU.
 */
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>

/* Mirror of drivers/staging/ethosu/uapi/ethosu.h (kept self-contained). */
#define ETHOSU_IOCTL_BASE 0x01
#define ETHOSU_IOR(nr, type) _IOR(ETHOSU_IOCTL_BASE, nr, type)

struct ethosu_uapi_device_hw_id {
	uint32_t version_status;
	uint32_t version_minor;
	uint32_t version_major;
	uint32_t product_major;
	uint32_t arch_patch_rev;
	uint32_t arch_minor_rev;
	uint32_t arch_major_rev;
};

struct ethosu_uapi_device_hw_cfg {
	uint32_t macs_per_cc;
	uint32_t cmd_stream_version;
	uint32_t custom_dma;
};

struct ethosu_uapi_device_capabilities {
	struct ethosu_uapi_device_hw_id  hw_id;
	struct ethosu_uapi_device_hw_cfg hw_cfg;
	uint32_t driver_patch_rev;
	uint32_t driver_minor_rev;
	uint32_t driver_major_rev;
};

#define ETHOSU_IOCTL_CAPABILITIES_REQ \
	ETHOSU_IOR(0x02, struct ethosu_uapi_device_capabilities)

int main(int argc, char **argv)
{
	const char *dev = (argc > 1) ? argv[1] : "/dev/ethosu0";
	struct ethosu_uapi_device_capabilities caps;
	int fd, ret;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		fprintf(stderr, "ethosu_caps: open(%s): %s\n",
			dev, strerror(errno));
		return 1;
	}

	memset(&caps, 0, sizeof(caps));
	ret = ioctl(fd, ETHOSU_IOCTL_CAPABILITIES_REQ, &caps);
	if (ret < 0) {
		fprintf(stderr, "ethosu_caps: CAPABILITIES_REQ: %s\n",
			strerror(errno));
		close(fd);
		return 1;
	}

	printf("=== Ethos-U capabilities (via %s) ===\n", dev);
	printf("hw_id.product_major   : %u\n", caps.hw_id.product_major);
	printf("hw_id.arch_major_rev  : %u\n", caps.hw_id.arch_major_rev);
	printf("hw_id.arch_minor_rev  : %u\n", caps.hw_id.arch_minor_rev);
	printf("hw_id.arch_patch_rev  : %u\n", caps.hw_id.arch_patch_rev);
	printf("hw_id.version_major   : %u\n", caps.hw_id.version_major);
	printf("hw_id.version_minor   : %u\n", caps.hw_id.version_minor);
	printf("hw_id.version_status  : %u\n", caps.hw_id.version_status);
	printf("hw_cfg.macs_per_cc    : %u\n", caps.hw_cfg.macs_per_cc);
	printf("hw_cfg.cmd_stream_ver : %u\n", caps.hw_cfg.cmd_stream_version);
	printf("hw_cfg.custom_dma     : %u\n", caps.hw_cfg.custom_dma);
	printf("driver %u.%u.%u\n", caps.driver_major_rev,
	       caps.driver_minor_rev, caps.driver_patch_rev);
	printf("=== CAPABILITIES_REQ OK ===\n");

	close(fd);
	return 0;
}
