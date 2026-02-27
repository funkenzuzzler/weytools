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

typedef enum {
	HP_CMD_WRITEGRAPH=0xa2,
	HP_CMD_READGRAPH=0xa3,
	HP_CMD_WRITEFILE=0xa5,
	HP_CMD_READFILE=0xa6,
	HP_CMD_DELETE=0xa8,
	HP_CMD_LISTFILES=0xa9,
} hp_cmds_t;

struct option options[] = {
	{ "device", required_argument, 0, 'D' },
	{ "baud", required_argument,   0, 'b' },
	{ "list", no_argument,         0, 'l' },
	{ "write", required_argument,  0, 'w' },
	{ "read", required_argument,   0, 'r' },
	{ "delete", required_argument, 0, 'd' }
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

static int listfiles(int fd)
{
	struct cmd_listfiles request = { .cmd = HP_CMD_LISTFILES, { 0 } };
	struct reply_listfile reply;
	struct fileentry *entries;
	int count, pktlen;

	if (write(fd, &request, sizeof(request)) == -1) {
		fprintf(stderr, "%s: send request: %m\n", __func__);
		return -1;
	}

	if (read_serial(fd, &reply, sizeof(reply)) == -1) {
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

	if (read_serial(fd, entries, pktlen) == -1) {
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

static int readgraphfile(int fd, int index, int subindex)
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
	if (write(fd, &request, sizeof(request)) == -1) {
		fprintf(stderr, "%s: send request: %m\n", __func__);
		return -1;
	}

	if (read_serial(fd, &status, sizeof(status)) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		return -1;
	}

	if (status != HP_CMD_READGRAPH) {
		fprintf(stderr, "%s: failed: %02x\n", __func__, ntohs(status));
		return -1;
	}

	/* read remaining 3 bytes before size */
	if (read_serial(fd, dummy, 4) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		return -1;
	}

	if (read_serial(fd, &size, sizeof(size)) == -1) {
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
		len = read_serial(fd, buf, MIN(sizeof(buf), size));
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

static int readfile(int fd, char *spec)
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
		return readgraphfile(fd, index, subindex);

	request.index = htons(index);
	request.subindex = htons(subindex);
	request.cmd = HP_CMD_READFILE;

	if (write(fd, &request, sizeof(request)) == -1) {
		fprintf(stderr, "%s: send request: %m\n", __func__);
		return -1;
	}

	if (read_serial(fd, &reply, sizeof(reply)) == -1) {
		fprintf(stderr, "%s: receive header: %m\n", __func__);
		return -1;
	}

	if (reply.cmd != HP_CMD_READFILE || ntohs(reply.status) >> 8 == 0xd0) {
		fprintf(stderr, "%s: failed: %04x\n", __func__, ntohs(reply.status));
		return -1;
	}

	reply2.name[0] = ((uint8_t *)&reply.status)[0];
	reply2.name[1] = ((uint8_t *)&reply.status)[1];

	if (read_serial(fd, &reply2.name[2], sizeof(reply2)-2) == -1) {
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
		len = read_serial(fd, buf, MIN(sizeof(buf), size));
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

static int deletefile(int fd, char *spec)
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

	if (write(fd, &request, sizeof(request)) == -1) {
		fprintf(stderr, "%s: send request: %m\n", __func__);
		return -1;
	}

	if (read_serial(fd, &reply, sizeof(reply)) == -1) {
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

static int writefile(int fd, char *spec)
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

	if (strlen(request.filename) > 31) {
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

	if (write(fd, &request, sizeof(request)) == -1) {
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

		ret = write(fd, buf, ret);
		if (ret == -1) {
			fprintf(stderr, "%s: send request: %m\n", __func__);
			goto out;
		}
		total -= ret;
		fprintf(stderr, "sent %d bytes, %ld remaining\n", ret, total);
	} while(total > 0);

	if (read_serial(fd, &reply,sizeof(reply)) == -1)
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

int main(int argc, char **argv)
{
	int list = 0, optidx, opt, baud = 115200;
	char *device = NULL, *endp, *write = NULL, *read = NULL, *delete = NULL;
	int ret = 1, fd;

	while ((opt = getopt_long(argc, argv, "lD:d:b:w:r:", options, &optidx)) != -1) {
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
		case 'h':
			fprintf(stderr, "%s: usage:%s <options>\n"
				"-d, --device            serial device\n"
				"-b, --baud,-b           baud rate\n"
				"-l, --list              list files on keyboard\n"
				"-w, --write <file>      upload file to keyboard\n"
				"-r, --read <file>       download file from keyboard\n",
				argv[0], argv[0]);
			return 0;
		default:
			break;
		}
	}

	if (!device) {
		fprintf(stderr, "missing device name\n");
		return 1;
	}

	fd = open_serial(device, baud);
	if (fd == -1)
		return 1;

	if (list) {
		ret = listfiles(fd);
		goto out;
	}

	if (read) {
		ret = readfile(fd, read);
		goto out;
	}

	if (write) {
		ret = writefile(fd, write);
		goto out;
	}
	if (delete) {
		ret = deletefile(fd, delete);
		goto out;
	}
out:
	close(fd);
	return ret;
}
