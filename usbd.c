#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/functionfs.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define NAME u8"IIO"

#define LE32(x) ((__BYTE_ORDER != __BIG_ENDIAN) ? (x) : __bswap_constant_32(x))
#define LE16(x) ((__BYTE_ORDER != __BIG_ENDIAN) ? (x) : __bswap_constant_16(x))

#define NB_PIPES 3

#define IIO_USD_CMD_RESET_PIPES 0
#define IIO_USD_CMD_OPEN_PIPE 1
#define IIO_USD_CMD_CLOSE_PIPE 2


/* ******************** */

struct usb_ffs_header {
	struct {
		struct usb_functionfs_descs_head_v2 header;
		uint32_t nb_fs, nb_hs, nb_ss;
	} __attribute__((packed));

	struct {
		struct usb_interface_descriptor intf;
		struct usb_endpoint_descriptor_no_audio  eps[NB_PIPES][2];
	} __attribute__((packed)) fs_descs, hs_descs, ss_descs;
} __attribute__((packed));

struct usb_ffs_strings {
	struct usb_functionfs_strings_head head;
	uint16_t lang;
	const char string[sizeof(NAME)];
} __attribute__((packed));

static const struct usb_ffs_header ffs_header = {
	.header = {
		.magic = LE32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2),
		.length = LE32(sizeof(ffs_header)),
		.flags = LE32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC |
				FUNCTIONFS_HAS_SS_DESC),
	},

	.nb_fs = LE32(NB_PIPES * 2 + 1),
	.nb_hs = LE32(NB_PIPES * 2 + 1),
	.nb_ss = LE32(NB_PIPES * 2 + 1),

#define EP(addr, packetsize) \
	{ \
	  .bLength = sizeof(struct usb_endpoint_descriptor_no_audio), \
	  .bDescriptorType = USB_DT_ENDPOINT, \
	  .bEndpointAddress = addr, \
	  .bmAttributes = USB_ENDPOINT_XFER_BULK, \
	  .wMaxPacketSize = LE16(packetsize), \
	}

#define EP_SET(id, packetsize) \
	[id] = { \
		[0] = EP((id + 1) | USB_DIR_IN, packetsize), \
		[1] = EP((id + 1) | USB_DIR_OUT, packetsize), \
	}

#define DESC(name, packetsize) \
	.name = {\
		.intf = { \
			.bLength = sizeof(ffs_header.name.intf), \
			.bDescriptorType = USB_DT_INTERFACE, \
			.bNumEndpoints = NB_PIPES * 2, \
			.bInterfaceClass = USB_CLASS_COMM, \
			.iInterface = 1, \
		}, \
		.eps = { \
			EP_SET(0, packetsize), \
			EP_SET(1, packetsize), \
			EP_SET(2, packetsize), \
		}, \
	}
	DESC(fs_descs, 64),
	DESC(hs_descs, 512),
	DESC(ss_descs, 512 /* no idea */),
#undef DESC
#undef EP
};

static const struct usb_ffs_strings ffs_strings = {
	.head = {
		.magic = LE32(FUNCTIONFS_STRINGS_MAGIC),
		.length = LE32(sizeof(ffs_strings)),
		.str_count = LE32(1),
		.lang_count = LE32(1),
	},

	.lang = LE16(0x409),
	.string = NAME,
};

static int write_header(int fd)
{
	int ret = write(fd, &ffs_header, sizeof(ffs_header));
	if (ret < 0)
		return ret;

	ret = write(fd, &ffs_strings, sizeof(ffs_strings));
	if (ret < 0)
		return ret;

	return 0;
}

static int stdio_redirect(const char *ep0, unsigned int i)
{
	int ret, ep;
	char buf[256], *end;

	strncpy(buf, ep0, sizeof(buf));
	end = strrchr(buf, '\0') - 1;

	snprintf(end, sizeof(buf) + buf - end, "%u", i * 2 + 1);
	ep = open(buf, O_WRONLY);
	if (ep < 0)
		return ep;

	ret = dup2(ep, STDOUT_FILENO);
	if (ret < 0)
		perror("Unable to redirect stdout\n");
	close(ep);

	snprintf(end, sizeof(buf) + buf - end, "%u", i * 2 + 2);
	ep = open(buf, O_RDONLY);
	if (ep < 0)
		return ep;

	ret = dup2(ep, STDIN_FILENO);
	if (ret < 0)
		perror("Unable to redirect stdin\n");
	close(ep);

	return 0;
}

struct usb_pipe {
	pid_t child;
};

static struct usb_pipe usb_pipes[NB_PIPES];
static char ep0_path[256];
static char **argv_new;

static void usb_open_pipe(unsigned int ep)
{
	struct usb_pipe *pipe;
	int child;

	if (ep >= NB_PIPES)
		return;

	pipe = &usb_pipes[ep];

	if (pipe->child)
		return;

	child = fork();
	if (child) {
		pipe->child = child;
		return;
	}

	if (stdio_redirect(ep0_path, ep)) {
		fprintf(stderr, "Unable to do IO redirection\n");
		exit(EXIT_FAILURE);
	}

	if (execvp(*argv_new, argv_new) < 0) {
		fprintf(stderr, "Execvp failed\n");
		exit(EXIT_FAILURE);
	}
}

static void usb_close_pipe(unsigned int ep)
{
	struct usb_pipe *pipe;

	if (ep >= NB_PIPES)
		return;

	pipe = &usb_pipes[ep];

	if (pipe->child == 0)
		return;

	kill(pipe->child, SIGTERM);
	/* Anytime soon now */
	waitpid(pipe->child, NULL, 0);

	pipe->child = 0;
}


static void handle_setup(struct usb_ctrlrequest *req)
{
	unsigned int i;

	switch (req->bRequest) {
	case IIO_USD_CMD_RESET_PIPES:
		for (i = 0; i < NB_PIPES; i++)
			usb_close_pipe(i);
		break;
	case IIO_USD_CMD_OPEN_PIPE:
		usb_open_pipe(le16toh(req->wValue));
		break;
	case IIO_USD_CMD_CLOSE_PIPE:
		usb_close_pipe(le16toh(req->wValue));
		break;
	}
}

static void handle_event(int fd, int argc, char **argv,
		struct usb_functionfs_event *event)
{
	switch (event->type) {
	case FUNCTIONFS_BIND:
		printf("Got event: BIND\n");
		break;
	case FUNCTIONFS_UNBIND:
		printf("Got event: UNBIND\n");
		break;
	case FUNCTIONFS_ENABLE:
		printf("Got event: ENABLE\n");
		break;
	case FUNCTIONFS_DISABLE:
		printf("Got event: DISABLE\n");
		break;
	case FUNCTIONFS_SETUP:
/*		printf("Got event: SETUP\n");*/
		handle_setup(&event->u.setup);
		read(fd, NULL, 0);
		break;
	case FUNCTIONFS_SUSPEND:
		printf("Got event: SUSPEND\n");
		break;
	case FUNCTIONFS_RESUME:
		printf("Got event: RESUME\n");
	default:
		break;
	}
}

int main(int argc, char **argv)
{
	int fd, ret;
	unsigned int i;

	if (argc < 3) {
		printf("Usage: %s ep0_node cmd ...\n", argv[0]);
		return EXIT_FAILURE;
	}

	argv_new = malloc((argc - 1) * sizeof(*argv));
	if (!argv_new)
		return EXIT_FAILURE;

	memcpy(argv_new, argv + 2, (argc - 2) * sizeof(*argv));
	argv_new[argc - 2] = NULL;

	snprintf(ep0_path, sizeof(ep0_path), "%s", argv[1]);

	fd = open(ep0_path, O_RDWR);
	if (fd < 0) {
		perror("Unable to open ep0 node");
		return EXIT_FAILURE;
	}

	ret = write_header(fd);
	if (ret < 0) {
		perror("Unable to write descriptor");
		return EXIT_FAILURE;
	}

	while (1) {
		struct usb_functionfs_event event;
		struct pollfd pollfd = {
			.fd = fd,
			.events = POLLIN,
		};

		ret = poll(&pollfd, 1, 100);
		if (ret < 0) {
			perror("Unable to poll ep0");
			ret = EXIT_FAILURE;
			break;
		}

		if (!ret)
			continue;

		ret = read(fd, &event, sizeof(event));
		if (ret != sizeof(event)) {
			fprintf(stderr, "Short read!\n");
			continue;
		}

		handle_event(fd, argc, argv, &event);
	}

	for (i = 0; i < NB_PIPES; i++)
		usb_close_pipe(i);

	free(argv_new);

	close(fd);
	return ret;
}
