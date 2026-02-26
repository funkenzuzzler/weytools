#include <stdio.h>
#include <libusb-1.0/libusb.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <arpa/inet.h>

#define MIN(a, b)  (((a) < (b)) ? (a) : (b))

struct module_info {
	char magic[4]; // MK06
	int number;
	char name[64];
	char date[12];
	uint8_t unknown[2];
	uint32_t base;
	uint32_t end;
	uint32_t csum;
} __attribute__((packed));

static void hexdump_line(char *out, uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < 16; i++) {
		if (!(i % 4))
			*out++ = ' ';
		if (i < len)
			sprintf(out, "%02X ", buf[i]);
		else
			memset(out, ' ', 3);
		out += 3;
	}

	for (size_t i = 0; i < len; i++) {
		char c = buf[i];
		if (c < 0x20)
			c = '.';
		sprintf(out++, "%c", c);
	}
}

void hexdump(char *prefix, void *buf, size_t len)
{
	char out[128] = { 0 };

	for (size_t offset = 0; offset < len; offset += 16) {
		hexdump_line(out, buf + offset, MIN(len - offset, 16));
		printf("%s: %04x: %s\n", prefix, (int)offset, out);
	}
}

static int restart(libusb_device_handle *dev, int mode)
{
	uint8_t restart[] = { 0xa0, 's', 0, 0, 0, mode };
	int ret, sent = 0;

	ret = libusb_bulk_transfer(dev, 0x06, restart, sizeof(restart), &sent, 1000);
	if (ret < 0)
		return ret;

	if (sent != sizeof(restart))
		return LIBUSB_ERROR_COUNT;
	return 0;
}

static int unlock(libusb_device_handle *dev)
{
	uint8_t cmd[] = { "\x7f\xe0gMk_eLeCtRoNiC-DeSiGn_gMbH-WeRnB" };
	uint8_t response[256];
	int ret, sent = 0;

	ret = libusb_bulk_transfer(dev, 0x06, cmd, sizeof(cmd), &sent, 1000);
	if (ret < 0) {
		fprintf(stderr, "%s: failed to send USB request: %s\n", __func__, libusb_strerror(ret));
		return ret;
	}

	ret = libusb_bulk_transfer(dev, 0x85, response, sizeof(response), &sent, 1000);
	if (ret < 0) {
		fprintf(stderr, "%s: failed to receive USB request: %s\n", __func__, libusb_strerror(ret));
		return ret;
	}

	if (sent != 5 || memcmp(response, "\x7f\xe0GMK", 5)) {
		fprintf(stderr, "%s: received invalid response\n", __func__);
		return LIBUSB_ERROR_IO;
	}
	return 0;
}

static int getid(libusb_device_handle *dev)
{
	uint8_t cmd[] = { "\xa0pID    " };
	uint8_t response[256];
	int ret, sent = 0;

	ret = libusb_bulk_transfer(dev, 0x06, cmd, sizeof(cmd), &sent, 1000);
	if (ret < 0) {
		fprintf(stderr, "%s: failed to send USB request: %s\n", __func__, libusb_strerror(ret));
		return ret;
	}

	ret = libusb_bulk_transfer(dev, 0x85, response, sizeof(response), &sent, 1000);
	if (ret < 0) {
		fprintf(stderr, "%s: failed to receive USB request: %s\n", __func__, libusb_strerror(ret));
		return ret;
	}

	if (sent < 8 || memcmp(response, "\xa0pID    ", 8)) {
		fprintf(stderr, "%s: received invalid response\n", __func__);
		return LIBUSB_ERROR_IO;
	}
	printf("Keyboard ID: %.*s\n", sent - 8, response + 8);
	return 0;
}

struct readcmd {
	uint8_t magic;
	char cmd[7] __attribute__((nonstring));
	uint32_t base;
	uint32_t len;
} __attribute__((packed));

static int readmem(libusb_device_handle *dev, uint32_t base, int len, uint8_t *out)
{
	struct readcmd cmd = { 0xa0, "pREAD  ", htonl(base), htonl(len) };
	int ret, sent = 0;

	hexdump("CMD", &cmd, sizeof(cmd));
	ret = libusb_bulk_transfer(dev, 0x06, (uint8_t *)&cmd, sizeof(cmd), &sent, 1000);
	if (ret < 0) {
		fprintf(stderr, "%s: failed to send USB request: %s\n", __func__, libusb_strerror(ret));
		return ret;
	}
	fprintf(stderr, "%s: sent %d bytes\n", __func__, sent);

	for (;;) {
		ret = libusb_bulk_transfer(dev, 0x85, out, MIN(len, 4096), &sent, 10000000);
		if (ret < 0) {
			fprintf(stderr, "%s: failed to receive USB request: %s\n", __func__, libusb_strerror(ret));
			return ret;
		}
		out += sent;
		len -= sent;
		if (sent < 64 || len <= 0)
			break;
	}

	return 0;
}

static int get_module_info(libusb_device_handle *dev, int id, struct module_info *info)
{
	uint8_t cmd[] = { 0xa0, 'q', 0, 0, 0, id };
	uint8_t response[260] = { 0 }, *p;
	int ret, total = 0, sent = 0;

	ret = libusb_bulk_transfer(dev, 0x06, cmd, sizeof(cmd), &sent, 1000);
	if (ret < 0) {
		fprintf(stderr, "%s: failed to send USB request: %s\n", __func__, libusb_strerror(ret));
		return ret;
	}

	p = response;
	for (;;) {
		ret = libusb_bulk_transfer(dev, 0x85, p, sizeof(response), &sent, 1000);
		if (ret < 0) {
			fprintf(stderr, "%s: failed to receive USB request: %s\n", __func__, libusb_strerror(ret));
			return ret;
		}
		total += sent;
		p += sent;
		if (sent < 64)
			break;
	}

	if (total != 258 || response[0] != 0xa0 || response[1] != 0x71) {
		fprintf(stderr, "%s: received invalid response\n", __func__);
		printf("\n");
		return LIBUSB_ERROR_IO;
	}

	if (strncmp((char *)response + 2, "MK06", 4))
		return -1;
	memcpy(info, response + 2, sizeof(struct module_info));
	return 0;
}

static libusb_device_handle *open_keyboard(struct libusb_context *ctx, int id)
{
	libusb_device_handle *dev = libusb_open_device_with_vid_pid(ctx, 0x0744, id);
	int ret;

	if (!dev) {
		fprintf(stderr, "libusb_open_device_with_vid_pid failed\n");
		return NULL;
	}

	libusb_set_configuration(dev, 1);
	ret = libusb_claim_interface(dev, 1);
	if (ret < 0) {
		fprintf(stderr, "libusb_claim_interface failed: %d\n", ret);
		return NULL;
	}
	return dev;
}

int main(int argc, char **argv)
{
	uint8_t dynblcmd[] = { 0x7f, 0xee, 'g', 'o', '-', 'D', 'y', 'n', 'B','l' };
	struct libusb_context *ctx;
	libusb_device_handle *dev;
	struct module_info info;
	uint8_t buf2[4096] = { 0 };
	int sent = 0;

	(void)argc;
	(void)argv;

	int ret = libusb_init(&ctx);
	if (ret < 0) {
		fprintf(stderr, "libusb_init failed: %s\n", libusb_strerror(ret));
		return 1;
	}

	dev = open_keyboard(ctx, 0x3f);
	if (!dev)
		goto out_exit;

	// send enter bootloader request
	ret = libusb_bulk_transfer(dev, 0x06, dynblcmd, sizeof(dynblcmd), &sent, 1000);
	libusb_release_interface(dev, 0);
	if (ret < 0 || sent != sizeof(dynblcmd))
		goto out_exit;
	sleep(1);
	dev = open_keyboard(ctx, 0x3e);
	if (!dev)
		goto out_exit;

	for (int i = 0; i < 64; i++) {
		if (get_module_info(dev, i, &info) < 0)
			continue;
		printf("%2d: %08x - %08x %s\n", i, ntohl(info.base), ntohl(info.end), info.name);
	}

	if (unlock(dev) < 0)
		goto out_release;

	if (getid(dev) < 0)
		goto out_release;
	readmem(dev, 0, 256, buf2);
	hexdump("BUF", buf2, sizeof(buf2));
	restart(dev, 5);
out_release:
	libusb_release_interface(dev, 0);
out_exit:
	libusb_exit(ctx);
	return 0;
}
