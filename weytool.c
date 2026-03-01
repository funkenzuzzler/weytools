#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <getopt.h>
#include <stdlib.h>
#include <termios.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <libusb-1.0/libusb.h>
#include <errno.h>

static int kbfd = -1;
static int verbose;

libusb_device_handle *usbdev;

typedef enum {
	HP_CMD_WRITEGRAPH=0xa2,
	HP_CMD_READGRAPH=0xa3,
	HP_CMD_WRITEFILE=0xa5,
	HP_CMD_READFILE=0xa6,
	HP_CMD_DELETE=0xa8,
	HP_CMD_LISTFILES=0xa9,
} hp_cmds_t;

typedef enum {
	OPT_RAWCMD = 0x100,
	OPT_RAWRX,
} optnum_t;

struct option options[] = {
	{ "device", required_argument, 0, 'D' },
	{ "baud", required_argument,   0, 'b' },
	{ "list", no_argument,         0, 'l' },
	{ "write", required_argument,  0, 'w' },
	{ "read", required_argument,   0, 'r' },
	{ "delete", required_argument, 0, 'd' },
	{ "reboot", no_argument,       0, 'R' },
	{ "verbose", no_argument,      0, 'v' },
	{ "rawcmd", required_argument, 0, OPT_RAWCMD },
	{ "rawrx", required_argument,  0, OPT_RAWRX },
};

struct cmd_listfiles {
	uint8_t cmd;
	uint8_t unused[3];
};

struct fileentry {
	uint16_t index;
	uint16_t subindex;
	char name[32];
};

struct reply_listfile {
	uint8_t cmd;
	uint8_t unused[2];
	uint32_t length;
	uint32_t count;
} __attribute__((packed));

struct request_fileread {
	uint8_t cmd;
	uint16_t index;
	uint16_t subindex;
} __attribute__((packed));

struct request_filedelete {
	uint8_t cmd;
	uint16_t index;
	uint16_t subindex;
} __attribute__((packed));

struct request_filewrite {
	uint8_t cmd;
	uint16_t index;
	uint16_t subindex;
	char filename[32];
	uint32_t size;
} __attribute__((packed));

struct reply_fileop {
	uint8_t cmd;
	uint16_t index;
	uint16_t subindex;
	uint16_t status;
} __attribute__((packed));

struct reply_fileread {
	char name[32];
	uint32_t size;
} __attribute__((packed));

struct request_graphfileread {
	uint8_t cmd;
	uint16_t magic;
	uint16_t subindex;
	uint32_t maxsize;
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

	if (!verbose)
		return;

	for (size_t offset = 0; offset < len; offset += 16) {
		hexdump_line(out, buf + offset, MIN(len - offset, 16));
		fprintf(stderr, "%s: %04x: %s\n", prefix, (int)offset, out);
	}
}

static int open_serial(char *device, int baud)
{
	int fd = open(device, O_RDWR);
	struct termios tty;

	if (fd == -1) {
		fprintf(stderr, "open %s: %m\n", device);
		return -1;
	}

	if (tcgetattr(fd, &tty) == -1) {
		fprintf(stderr, "tcgetattr: %m\n");
		goto err;
	}

	cfsetispeed(&tty, baud);
	cfsetospeed(&tty, baud);

	tty.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
	tty.c_cflag |= (CS8 | CLOCAL | CREAD);

	tty.c_iflag &= ~(IGNBRK | IXON | IXOFF | IXANY);
	tty.c_oflag = 0;

	if (tcsetattr(fd, TCSANOW, &tty) == -1) {
		fprintf(stderr, "tcsetattr: %m\n");
		goto err;
	}
	return fd;
err:
	close(fd);
	return -1;
}

static ssize_t read_serial(int fd, void *buf, ssize_t count)
{
	ssize_t ret, total = 0;

	do {
		ret = read(fd, buf, count);
		if (ret == -1) {
			fprintf(stderr, "%s: %m\n", __func__);
			return -1;
		}

		if (!ret) {
			fprintf(stderr, "%s: unexpected EOF\n", __func__);
			return -1;
		}

		if (ret > 0) {
			buf += ret;
			count -= ret;
			total += ret;
		}
	} while (count > 0);
	return total;
}

static int read_keyboard(void *buf, size_t count)
{
	static uint8_t tmpbuf[4096];
	static size_t rxavailable;
	int sent, ret;

	if (kbfd != -1)
		return read_serial(kbfd, buf, count);

	if (count > sizeof(tmpbuf))
		count = sizeof(tmpbuf);

	while(rxavailable < count) {
		ret = libusb_bulk_transfer(usbdev, 0x85, tmpbuf + rxavailable,
					   sizeof(tmpbuf) - rxavailable, &sent, 60000);
		switch (ret) {
		case LIBUSB_SUCCESS:
			rxavailable += sent;
			break;
		default:
			fprintf(stderr, "%s: %s\n", __func__, libusb_strerror(ret));
			errno = EIO;
			return -1;
		}
	}

	rxavailable -= count;
	memcpy(buf, tmpbuf, count);
	hexdump("RX", buf, count);
	memmove(tmpbuf, tmpbuf + count, rxavailable);
	return count;
}

static int write_keyboard(void *buf, size_t count)
{
	int sent, ret, total = 0;

	hexdump("TX", buf, count);
	if (kbfd != -1)
		return write(kbfd, buf, count);
	do {
		ret = libusb_bulk_transfer(usbdev, 0x06, buf, MIN(count, 64), &sent, 60000);
		switch (ret) {
		case LIBUSB_SUCCESS:
			count -= sent;
			buf += sent;
			total += sent;
			break;
		default:
			fprintf(stderr, "%s: %s, sent %d\n", __func__, libusb_strerror(ret), sent);
			errno = EIO;
			return -1;
		}
	} while(count);

	return total;
}

static int enter_usb_mode(void)
{
	uint8_t dynblcmd[] = { 0x7f, 0xf0, 'm', 'o', 'd', 'e', '-', 'u', 's', 'b' };

	return write_keyboard(dynblcmd, sizeof(dynblcmd));
}


static int listfiles(void)
{
	struct cmd_listfiles request = { .cmd = HP_CMD_LISTFILES, { 0 } };
	struct reply_listfile reply;
	struct fileentry *entries;
	int count, pktlen;

	if (write_keyboard(&request, sizeof(request)) == -1) {
		fprintf(stderr, "%s: send request: %m\n", __func__);
		return -1;
	}

	if (read_keyboard(&reply, sizeof(reply)) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		return -1;
	}

	count = htonl(reply.count);
	pktlen = count * sizeof(*entries);
	if (!pktlen || pktlen > 1048576) {
		fprintf(stderr, "unexpected pktlen: %d\n", pktlen);
		return -1;
	}

	entries = malloc(pktlen);
	if (!entries) {
		fprintf(stderr, "out of memory\n");
		return -1;
	}

	if (read_keyboard(entries, pktlen) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		free(entries);
		return -1;
	}

	printf("Number Index SubIndex Name\n");
	for (int i = 0; i < count; i++)
		printf("%6d %5d %8d %s\n", i, htons(entries[i].index), htons(entries[i].subindex), entries[i].name);
	free(entries);
	return 0;
}

static int readgraphfile(int index, int subindex)
{
	struct request_graphfileread request;
	int outfd, ret = -1;
	char buf[512];
	uint8_t status;
	uint32_t size;
	ssize_t len;
	char name[32];
	char dummy[8];
	int total;

	switch (index) {
	case 4:
		request.magic = htons(0xa054);
		request.subindex = htons((subindex + 0x70) << 8);
		sprintf(name, "BMP%d.BMP", subindex);
		break;
	case 6:
		request.magic = htons(0x0101);
		request.subindex = htons(subindex);
		strcpy(name, "Colorparm.par");
		break;
	default:
		return -1;
	}

	request.cmd = HP_CMD_READGRAPH;
	request.maxsize = htonl(1000000);
	if (write_keyboard(&request, sizeof(request)) == -1) {
		fprintf(stderr, "%s: send request: %m\n", __func__);
		return -1;
	}

	if (read_keyboard(&status, sizeof(status)) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		return -1;
	}

	if (status != HP_CMD_READGRAPH) {
		fprintf(stderr, "%s: failed: %02x\n", __func__, ntohs(status));
		return -1;
	}

	/* read remaining 3 bytes before size */
	if (read_keyboard(dummy, 4) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		return -1;
	}

	if (read_keyboard(&size, sizeof(size)) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		return -1;
	}

	size = ntohl(size);
	total = size;
	printf("%s: %d bytes\n", name, size);

	outfd = open(name, O_RDWR|O_CREAT|O_TRUNC, 0644);
	if (outfd == -1) {
		fprintf(stderr, "%s: failed to create output file %s: %m\n", __func__, name);
		return -1;
	}

	do {
		len = read_keyboard(buf, MIN(sizeof(buf), size));
		if (len == -1)
			goto out;
		if (write(outfd, buf, len) == -1)
			goto out;
		size -= len;
		printf("%5.1f%% done\r", ((float)(total - size) / (float)total) * 100);
	} while(size > 0);
	printf("\n");
	ret = 0;
out:
	close(outfd);
	return ret;
}

static int readfile(char *spec)
{
	struct request_fileread request;
	struct reply_fileop reply;
	struct reply_fileread reply2;
	int index, subindex, outfd, ret = -1, size;
	char buf[512];
	ssize_t len;

	if (sscanf(spec, "%d,%d", &index, &subindex) != 2) {
		fprintf(stderr, "%s: invalid spec: %s\n", __func__, spec);
		return -1;
	}

	if (index == 4 || index == 6)
		return readgraphfile(index, subindex);

	request.index = htons(index);
	request.subindex = htons(subindex);
	request.cmd = HP_CMD_READFILE;

	if (write_keyboard(&request, sizeof(request)) == -1) {
		fprintf(stderr, "%s: send request: %m\n", __func__);
		return -1;
	}

	if (read_keyboard(&reply, sizeof(reply)) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		return -1;
	}

	if (reply.cmd != HP_CMD_READFILE || ntohs(reply.status) >> 8 == 0xd0) {
		fprintf(stderr, "%s: failed: %04x\n", __func__, ntohs(reply.status));
		return -1;
	}

	reply2.name[0] = ((uint8_t *)&reply.status)[0];
	reply2.name[1] = ((uint8_t *)&reply.status)[1];

	if (read_keyboard(&reply2.name[2], sizeof(reply2)-2) == -1) {
		fprintf(stderr, "%s: receive header2: %m\n", __func__);
		return -1;
	}

	size = htonl(reply2.size);
	printf("%d,%d: %s %d bytes\n", ntohs(reply.index), ntohs(reply.subindex), reply2.name, size);

	outfd = open(reply2.name, O_RDWR|O_CREAT|O_TRUNC, 0644);
	if (outfd == -1) {
		fprintf(stderr, "%s: failed to create output file %s: %m\n", __func__, reply2.name);
		return -1;
	}

	do {
		len = read_keyboard(buf, MIN(sizeof(buf), size));
		if (len == -1)
			goto out;
		if (write(outfd, buf, len) == -1)
			goto out;
		size -= len;
	} while(size > 0);
	ret = 0;
out:
	close(outfd);
	return ret;
}

static int deletefile(char *spec)
{
	struct request_filedelete request;
	struct reply_fileop reply;
	int index, subindex, ret = -1;

	if (sscanf(spec, "%d,%d", &index, &subindex) != 2) {
		fprintf(stderr, "%s: invalid spec: %s\n", __func__, spec);
		return -1;
	}

	request.index = htons(index);
	request.subindex = htons(subindex);
	request.cmd = HP_CMD_DELETE;

	if (write_keyboard(&request, sizeof(request)) == -1) {
		fprintf(stderr, "%s: send request: %m\n", __func__);
		return -1;
	}

	if (read_keyboard(&reply, sizeof(reply)) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		return -1;
	}
	ret = 0;

	if (reply.cmd != HP_CMD_DELETE || ntohs(reply.status) != 0xd000) {
		fprintf(stderr, "%s: delete failed: %04x\n", __func__,
			ntohs(reply.status));
		goto out;
	}

out:
	return ret;
}

static int writefile(char *spec)
{
	int index, subindex, ret = -1, infd;
	struct request_filewrite request;
	struct reply_fileop reply;
	char input[32], buf[512];
	struct stat statbuf;
	ssize_t total;

	if (sscanf(spec, "LAYER%02d.LAY", &subindex) == 1) {
		index = 9;
		strcpy(input, spec);
	} else if (sscanf(spec, "%d,%d,%s", &index, &subindex, input) != 3) {
		fprintf(stderr, "%s: invalid spec: %s\n", __func__, spec);
		return -1;
	}

	if (strlen(input) > 31) {
		fprintf(stderr, "%s: filename %s too long\n", __func__, input);
		return -1;
	}

	infd = open(input, O_RDONLY);
	if (infd == -1) {
		fprintf(stderr, "%s: failed to open %s: %m\n", __func__, input);
		return -1;
	}

	if (fstat(infd, &statbuf) == -1) {
		fprintf(stderr, "fstat: %m\n");
		goto out;
	}

	total = statbuf.st_size;
	memset(&request, 0, sizeof(request));
	strncpy(request.filename, input, sizeof(request.filename)-1);
	request.index = htons(index);
	request.subindex = htons(subindex);
	request.size = htonl(statbuf.st_size);
	request.cmd = HP_CMD_WRITEFILE;

	if (write_keyboard(&request, sizeof(request)) == -1) {
		fprintf(stderr, "%s: failed to write request: %m\n", __func__);
		goto out;
	}

	do {
		ret = read(infd, buf, MIN(total, sizeof(buf)));
		if (ret == -1) {
			fprintf(stderr, "%s: read: %m\n", __func__);
			goto out;
		}

		if (ret == 0)
			break;

		ret = write_keyboard(buf, ret);
		if (ret == -1) {
			fprintf(stderr, "%s: send request: %m\n", __func__);
			goto out;
		}

		total -= ret;
		fprintf(stderr, "sent %d bytes, %ld remaining\n", ret, total);
	} while(total > 0);

	if (read_keyboard(&reply,sizeof(reply)) == -1)
		goto out;

	if (reply.cmd != HP_CMD_WRITEFILE || ntohs(reply.status) != 0xd000) {
		fprintf(stderr, "%s: %s: failed: %04x\n", __func__,
			input, ntohs(reply.status));
		goto out;
	}
	ret = 0;
out:
	close(infd);
	return ret;
}

static int reboot_kbd(void)
{
	uint8_t cmd[] = { 0x7f, 0xe4, 0x31, 0xc0, 0x02 };

	if (write_keyboard(cmd, sizeof(cmd)) == 1) {
		fprintf(stderr, "%s: %m\n", __func__);
		return -1;
	}
	return 0;
}

static int rawtx(uint8_t *buf, int size)
{
	return write_keyboard(buf, size);
}

static int rawrx(int size)
{
	char *buf;
	int ret;

	if (size > 1048576) {
		fprintf(stderr, "%s: size exceeds limit of 1MB\n", __func__);
		return -1;
	}
	buf = malloc(size);
	if (!buf) {
		fprintf(stderr, "%s: failed to allocate rx buffer\n", __func__);
		return -1;
	}
	ret = read_keyboard(buf, size);
	if (ret) {
		free(buf);
		return ret;
	}
	hexdump("RX", buf, size);
	free(buf);
	return 0;
}

static libusb_device_handle *open_keyboard_usb(struct libusb_context *ctx, int id)
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

static uint8_t *parse_rawcmd(char *arg, int *rawcount)
{
	char *endp, *p, *_arg = arg;
	static uint8_t buf[128];
	size_t cnt = 0;

	*rawcount = 0;

	while ((p = strtok(_arg, ";,"))) {
		buf[cnt++] = strtoul(p, &endp, 16);
		if (*endp) {
			fprintf(stderr, "%s: failed to parse `%s'\n", __func__, endp);
			return NULL;
		}

		if (cnt > sizeof(buf)) {
			fprintf(stderr, "%s: raw cmd list exceeds size of %ld bytes\n",
				__func__, sizeof(buf));
			return NULL;
		}
		_arg = NULL;
	}
	*rawcount = cnt;
	return buf;
}

int main(int argc, char **argv)
{
	char *device = NULL, *endp, *write = NULL, *read = NULL, *delete = NULL;
	int list = 0, optidx, opt, baud = 115200;
	struct libusb_context *ctx = NULL;
	int ret = 1, reboot = 0, rawtxsize = 0, rawrxsize = 0;
	uint8_t *rawlist;

	while ((opt = getopt_long(argc, argv, "hvRlD:d:b:w:r:", options, &optidx)) != -1) {
		 switch (opt) {
		 case 'D':
			 device = optarg;
			 break;
		 case 'b':
			 baud = strtoul(optarg, &endp, 10);
			 if (*endp) {
				 fprintf(stderr, "invalid baudrate: %s\n", optarg);
				 return 1;
			 }
			 break;
		 case 'l':
			 list = 1;
			 break;
		 case 'w':
			 write = optarg;
			 break;
		 case 'r':
			 read = optarg;
			 break;
		 case 'd':
			 delete = optarg;
			 break;
		 case 'v':
			 verbose = 1;
			 break;
		 case 'R':
			 reboot = 1;
			 break;
		 case OPT_RAWCMD:
			 rawlist = parse_rawcmd(optarg, &rawtxsize);
			 if (!rawlist)
				 return 1;
			 break;
		 case OPT_RAWRX:
			 rawrxsize = strtoul(optarg, &endp, 10);
			 if (*endp) {
				 fprintf(stderr, "invalid raw RX size: %s\n", optarg);
				 return 1;
			 }
			 break;
		 case 'h':
			 fprintf(stderr, "%s: usage:%s <options>\n"
				 "-D, --device            serial device\n"
				 "-b, --baud,-b           baud rate\n"
				 "-l, --list              list files on keyboard\n"
				 "-w, --write <file>      upload file to keyboard\n"
				 "-r, --read <file>       download file from keyboard\n"
				 "-d, --delete <file>     delete file from keyboard\n"
				 "-R, --reboot            reboot keyboard\n"
				 "-v, --verbose           log data transfers\n"
				 "    --rawcmd <hexbytes> send raw cmd to keyboard\n"
				 "    --rawrx <len>       receive raw response from keyboard\n",
				 argv[0], argv[0]);
			 return 0;
		 default:
			 break;
		 }
	 }

	 /*
	  * If the keyboard we're talking to is the keyboard control this pc,
	  * we might block the keyboard before it could send the key up event.
	  * This leads to repeated keypresses until we're done. Sleep for one
	  * second to minimize the risk.
	  */
	sleep(1);

	if (!device) {
		/* no serial device give, try usb */
		int ret = libusb_init(&ctx);
		if (ret < 0) {
			fprintf(stderr, "libusb_init failed: %s\n", libusb_strerror(ret));
			return 1;
		}
		usbdev = open_keyboard_usb(ctx, 0x3f);
		if (!usbdev)
			goto out_release;
		if (enter_usb_mode() == -1)
			goto out_release;

		sleep(1);
	} else {
		kbfd = open_serial(device, baud);
		if (kbfd == -1)
			return 1;
	}

	if (list) {
		ret = listfiles();
		if (ret == -1)
			goto out;
	}

	if (delete) {
		ret = deletefile(delete);
		if (ret == -1)
			goto out;
	}

	if (read) {
		ret = readfile(read);
		if (ret == -1)
			goto out;
	}

	if (write) {
		ret = writefile(write);
		if (ret == -1)
			goto out;
	}

	if (rawtxsize) {
		ret = rawtx(rawlist, rawtxsize);
		if (ret == -1)
		goto out;
	}

	if (rawrxsize) {
		ret = rawrx(rawrxsize);
		if (ret == -1)
			goto out;
	}

	if (reboot)
		ret = reboot_kbd();
out_release:
	if (ctx)
		libusb_exit(ctx);
out:
	if (kbfd != -1)
		close(kbfd);
	return ret == 0 ? 0 : 1;
}
