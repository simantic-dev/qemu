// SPDX-License-Identifier: GPL-2.0-only
/*
 * ethosu_infer - run a full Ethos-U65 inference from Linux user space on the
 * QEMU i.MX93 machine (fork-only inference demo).
 *
 * Sequence (all via /dev/ethosu0 ioctls, no driver library needed):
 *   BUFFER_CREATE + mmap + memcpy(vela model) + BUFFER_SET   -> network buffer
 *   NETWORK_CREATE(type=BUFFER)                              -> network fd
 *   NETWORK_INFO                                             -> ifm/ofm sizes
 *   BUFFER_CREATE(ifm) + fill input                         -> ifm buffer
 *   BUFFER_CREATE(ofm)                                      -> ofm buffer
 *   INFERENCE_CREATE(ifm_fd, ofm_fd, memory_layout)         -> inference fd
 *   INFERENCE_INVOKE ; poll INFERENCE_STATUS until !RUNNING
 *   read OFM from the mmap'd ofm buffer, print + argmax
 *
 * Opening /dev/ethosu0 boots the M33 on demand (the i.MX SiP RPROC SMC the
 * machine services); the M33 firmware runs the inference, the in-QEMU Ethos-U
 * executor (hw/npu/) runs the command stream to produce the OFM, raises the
 * completion IRQ, and the result lands in the ofm buffer.
 *
 * Build: aarch64-linux-gnu-gcc -static -O2 -o ethosu_infer ethosu_infer.c
 * Not an upstream QEMU deliverable - it talks to a Linux char device.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/* ---- mirror of drivers/staging/ethosu/uapi/ethosu.h ---- */
#define ETHOSU_IOCTL_BASE 0x01
#define ETHOSU_IO(nr)        _IO(ETHOSU_IOCTL_BASE, nr)
#define ETHOSU_IOR(nr, t)    _IOR(ETHOSU_IOCTL_BASE, nr, t)
#define ETHOSU_FD_MAX 16
#define ETHOSU_DIM_MAX 8
#define ETHOSU_PMU_EVENT_MAX 4

struct ethosu_uapi_buffer_create { uint32_t capacity; };
struct ethosu_uapi_buffer { uint32_t offset; uint32_t size; };
struct ethosu_uapi_network_create { uint32_t type; union { uint32_t fd; uint32_t index; }; };
struct ethosu_uapi_network_info {
	char desc[32];
	uint32_t is_vela;
	uint32_t ifm_count;
	uint32_t ifm_size[ETHOSU_FD_MAX];
	uint32_t ifm_types[ETHOSU_FD_MAX];
	uint32_t ifm_offset[ETHOSU_FD_MAX];
	uint32_t ifm_dims[ETHOSU_FD_MAX];
	uint32_t ifm_shapes[ETHOSU_FD_MAX][ETHOSU_DIM_MAX];
	uint32_t ofm_count;
	uint32_t ofm_size[ETHOSU_FD_MAX];
	uint32_t ofm_types[ETHOSU_FD_MAX];
	uint32_t ofm_offset[ETHOSU_FD_MAX];
	uint32_t ofm_dims[ETHOSU_FD_MAX];
	uint32_t ofm_shapes[ETHOSU_FD_MAX][ETHOSU_DIM_MAX];
};
struct ethosu_uapi_pmu_config { uint32_t events[ETHOSU_PMU_EVENT_MAX]; uint32_t cycle_count; };
struct ethosu_uapi_pmu_counts { uint32_t events[ETHOSU_PMU_EVENT_MAX]; uint64_t cycle_count; };
struct ethosu_uapi_memory_layout {
	uint32_t flash_offset;
	uint32_t arena_offset;
	uint32_t input_count;
	uint32_t input_offset[ETHOSU_FD_MAX];
	uint32_t input_size[ETHOSU_FD_MAX];
	uint32_t output_count;
	uint32_t output_offset[ETHOSU_FD_MAX];
	uint32_t output_size[ETHOSU_FD_MAX];
};
struct ethosu_uapi_inference_create {
	uint32_t ifm_count;
	uint32_t ifm_fd[ETHOSU_FD_MAX];
	uint32_t ofm_count;
	uint32_t ofm_fd[ETHOSU_FD_MAX];
	struct ethosu_uapi_memory_layout memory_layout;
	uint32_t inference_type;     /* enum: 0 = MODEL */
	struct ethosu_uapi_pmu_config pmu_config;
};
struct ethosu_uapi_result_status {
	uint32_t status;             /* 0 OK,1 ERROR,2 RUNNING,3 REJECTED,4 ABORTED */
	struct ethosu_uapi_pmu_config pmu_config;
	struct ethosu_uapi_pmu_counts pmu_count;
};

#define ETHOSU_IOCTL_NETWORK_INFO     ETHOSU_IOR(0x21, struct ethosu_uapi_network_info)
#define ETHOSU_IOCTL_BUFFER_CREATE    ETHOSU_IOR(0x10, struct ethosu_uapi_buffer_create)
#define ETHOSU_IOCTL_BUFFER_SET       ETHOSU_IOR(0x11, struct ethosu_uapi_buffer)
#define ETHOSU_IOCTL_NETWORK_CREATE   ETHOSU_IOR(0x20, struct ethosu_uapi_network_create)
#define ETHOSU_IOCTL_INFERENCE_CREATE ETHOSU_IOR(0x30, struct ethosu_uapi_inference_create)
#define ETHOSU_IOCTL_INFERENCE_STATUS ETHOSU_IOR(0x31, struct ethosu_uapi_result_status)
#define ETHOSU_IOCTL_INFERENCE_INVOKE ETHOSU_IOR(0x33, struct ethosu_uapi_result_status)
#define ETHOSU_UAPI_NETWORK_BUFFER 1
/* ---- end uapi ---- */

static const char *st_str(uint32_t s)
{
	static const char *t[] = { "OK", "ERROR", "RUNNING", "REJECTED",
				   "ABORTED", "ABORTING" };
	return s < 6 ? t[s] : "?";
}

static void *slurp(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f) { fprintf(stderr, "open %s: %s\n", path, strerror(errno)); return NULL; }
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	void *p = malloc(n);
	if (fread(p, 1, n, f) != (size_t)n) { fclose(f); free(p); return NULL; }
	fclose(f); *len = n; return p;
}

/* BUFFER_CREATE + mmap; optionally copy `data` and BUFFER_SET its size. */
static int make_buffer(int dev, uint32_t cap, const void *data, size_t dlen,
		       void **map_out)
{
	struct ethosu_uapi_buffer_create bc = { .capacity = cap };
	int bfd = ioctl(dev, ETHOSU_IOCTL_BUFFER_CREATE, &bc);
	if (bfd < 0) { fprintf(stderr, "BUFFER_CREATE(%u): %s\n", cap, strerror(errno)); return -1; }
	void *m = mmap(NULL, cap, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
	if (m == MAP_FAILED) { fprintf(stderr, "mmap: %s\n", strerror(errno)); close(bfd); return -1; }
	if (data) {
		memcpy(m, data, dlen);
		struct ethosu_uapi_buffer b = { .offset = 0, .size = (uint32_t)dlen };
		if (ioctl(bfd, ETHOSU_IOCTL_BUFFER_SET, &b) < 0)
			fprintf(stderr, "BUFFER_SET: %s\n", strerror(errno));
	}
	*map_out = m;
	return bfd;
}

int main(int argc, char **argv)
{
	const char *modelp = argc > 1 ? argv[1] : "/lib/firmware/model_int8_vela.tflite";
	const char *ifmp   = argc > 2 ? argv[2] : "/sample.bin";
	const char *dev_p  = "/dev/ethosu0";

	size_t mlen = 0, ilen = 0;
	void *model = slurp(modelp, &mlen);
	void *ifm = slurp(ifmp, &ilen);
	if (!model || !ifm) return 1;
	printf("ethosu_infer: model=%s (%zuB) ifm=%s (%zuB)\n", modelp, mlen, ifmp, ilen);

	int dev = open(dev_p, O_RDWR);
	if (dev < 0) { fprintf(stderr, "open %s: %s\n", dev_p, strerror(errno)); return 1; }

	void *netmap;
	int netbuf = make_buffer(dev, (uint32_t)mlen, model, mlen, &netmap);
	if (netbuf < 0) return 1;

	struct ethosu_uapi_network_create nc = { .type = ETHOSU_UAPI_NETWORK_BUFFER };
	nc.fd = netbuf;
	int net = ioctl(dev, ETHOSU_IOCTL_NETWORK_CREATE, &nc);
	if (net < 0) { fprintf(stderr, "NETWORK_CREATE: %s\n", strerror(errno)); return 1; }
	printf("network fd=%d\n", net);

	struct ethosu_uapi_network_info ni;
	memset(&ni, 0, sizeof(ni));
	if (ioctl(net, ETHOSU_IOCTL_NETWORK_INFO, &ni) == 0) {
		printf("network: desc='%.31s' is_vela=%u\n"
		       "  ifm_count=%u ifm_size[0]=%u ifm_offset[0]=%u\n"
		       "  ofm_count=%u ofm_size[0]=%u ofm_offset[0]=%u\n",
		       ni.desc, ni.is_vela,
		       ni.ifm_count, ni.ifm_size[0], ni.ifm_offset[0],
		       ni.ofm_count, ni.ofm_size[0], ni.ofm_offset[0]);
	} else {
		fprintf(stderr, "NETWORK_INFO: %s (continuing)\n", strerror(errno));
	}

	uint32_t in_size  = (ni.ifm_count && ni.ifm_size[0]) ? ni.ifm_size[0] : (uint32_t)ilen;
	uint32_t out_size = (ni.ofm_count && ni.ofm_size[0]) ? ni.ofm_size[0] : 16;
	uint32_t in_off   = ni.ifm_offset[0];
	uint32_t out_off  = ni.ofm_offset[0];

	/*
	 * The IFM buffer IS the tflite-micro tensor arena (a SingleArenaBuffer).
	 * The input tensor lives at arena+ifm_offset and the output tensor at
	 * arena+ofm_offset (offsets from the model's offline memory plan, reported
	 * by NETWORK_INFO). Size the arena generously; override with ETHOSU_ARENA.
	 */
	uint32_t arena = 256 * 1024;
	const char *ae = getenv("ETHOSU_ARENA");
	if (ae && *ae) arena = (uint32_t)strtoul(ae, NULL, 0);

	void *arenamap, *ofmmap;
	int ifmbuf = make_buffer(dev, arena, NULL, 0, &arenamap);
	int ofmbuf = make_buffer(dev, out_size ? out_size : 16, NULL, 0, &ofmmap);
	if (ifmbuf < 0 || ofmbuf < 0) return 1;
	memset(arenamap, 0, arena);
	memset(ofmmap, 0, out_size ? out_size : 16);
	/* Place the input image into the arena at its planned offset. */
	memcpy((uint8_t *)arenamap + in_off, ifm, in_size < ilen ? in_size : ilen);
	{
		struct ethosu_uapi_buffer b = { .offset = 0, .size = arena };
		ioctl(ifmbuf, ETHOSU_IOCTL_BUFFER_SET, &b);
	}

	struct ethosu_uapi_inference_create ic;
	memset(&ic, 0, sizeof(ic));
	ic.ifm_count = 1; ic.ifm_fd[0] = ifmbuf;
	ic.ofm_count = 1; ic.ofm_fd[0] = ofmbuf;
	ic.inference_type = 0; /* MODEL */
	ic.memory_layout.input_count = 1;
	ic.memory_layout.input_offset[0] = in_off;
	ic.memory_layout.input_size[0] = in_size;
	ic.memory_layout.output_count = 1;
	ic.memory_layout.output_offset[0] = out_off;
	ic.memory_layout.output_size[0] = out_size;

	int inf = ioctl(net, ETHOSU_IOCTL_INFERENCE_CREATE, &ic);
	if (inf < 0) { fprintf(stderr, "INFERENCE_CREATE: %s\n", strerror(errno)); return 1; }
	printf("inference fd=%d, invoking...\n", inf);

	struct ethosu_uapi_result_status rs;
	memset(&rs, 0, sizeof(rs));
	if (ioctl(inf, ETHOSU_IOCTL_INFERENCE_INVOKE, &rs) < 0) {
		fprintf(stderr, "INFERENCE_INVOKE: %s\n", strerror(errno));
		return 1;
	}

	uint32_t status = 2; /* RUNNING */
	for (int i = 0; i < 1200; i++) {    /* up to ~60s (host TFLite is slow) */
		memset(&rs, 0, sizeof(rs));
		if (ioctl(inf, ETHOSU_IOCTL_INFERENCE_STATUS, &rs) < 0) {
			fprintf(stderr, "INFERENCE_STATUS: %s\n", strerror(errno));
			break;
		}
		status = rs.status;
		if (status != 2) break;
		usleep(50000);
	}
	printf("inference status: %s (%u)\n", st_str(status), status);

	/* Output may be copied to the OFM buffer and/or left in the arena. */
	int8_t *ob = (int8_t *)ofmmap;
	int8_t *oa = (int8_t *)arenamap + out_off;
	printf("OFM (ofm buffer, %u bytes):", out_size);
	for (uint32_t i = 0; i < out_size && i < 16; i++) printf(" %d", ob[i]);
	printf("\n");
	printf("OFM (arena +0x%x, %u bytes):", out_off, out_size);
	for (uint32_t i = 0; i < out_size && i < 16; i++) printf(" %d", oa[i]);
	printf("\n");
	if (out_size >= 2) {
		int8_t *o = (ob[0] || ob[1]) ? ob : oa;
		int cls = (o[1] > o[0]) ? 1 : 0;
		printf("argmax class = %d  (0=top-brighter, 1=bottom-brighter)\n", cls);
	}

	if (status == 0)
		printf("RESULT: INFERENCE OK\n");
	else
		printf("RESULT: INFERENCE FAILED (status=%s)\n", st_str(status));
	return status == 0 ? 0 : 1;
}
