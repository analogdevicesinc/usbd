#include <errno.h>
#include <fcntl.h>
#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUFSIZE 8192
#define NB_TRANSFERS 8

struct thd_data {
	libusb_device_handle *hdl;
	int ep;
	volatile bool thd_stop;
};

static bool usb_transfer_is_free(const struct libusb_transfer *transfer)
{
	return !transfer->user_data;
}

static void usb_transfer_set_free(struct libusb_transfer *transfer, bool free)
{
	transfer->user_data = (void *) (uintptr_t) !free;
}

static struct libusb_transfer * usb_alloc_transfer(libusb_device_handle *hdl,
		int ep, libusb_transfer_cb_fn callback, void *d)
{
	void *buffer;
	struct libusb_transfer *transfer = libusb_alloc_transfer(0);
	if (!transfer)
		return NULL;

	buffer = calloc(1, BUFSIZE);
	if (!buffer) {
		libusb_free_transfer(transfer);
		return NULL;
	}

	libusb_fill_bulk_transfer(transfer, hdl, ep, buffer, BUFSIZE,
			callback, d, 0);
	transfer->flags = LIBUSB_TRANSFER_FREE_BUFFER;
	return transfer;
}

static void usb_data_sent_cb(struct libusb_transfer *transfer)
{
	switch (transfer->status) {
	default:
		fprintf(stderr, "USB transfer error: %i\n", transfer->status);
	case LIBUSB_TRANSFER_CANCELLED:
		fprintf(stderr, "Transfer transmit cancelled!\n");
	case LIBUSB_TRANSFER_COMPLETED:
		break;
	}

	usb_transfer_set_free(transfer, true);
}

static void * write_thd(void *data)
{
	unsigned int i;
	struct thd_data *thd_data = (struct thd_data *) data;
	struct libusb_transfer * transfers[NB_TRANSFERS];

	for (i = 0; i < NB_TRANSFERS; i++) {
		transfers[i] = usb_alloc_transfer(thd_data->hdl, thd_data->ep,
				usb_data_sent_cb, NULL);
		usb_transfer_set_free(transfers[i], true);
	}

	while (!thd_data->thd_stop) {
		struct libusb_transfer *transfer = NULL;
		ssize_t count;

		while (true) {
			for (i = 0; i < NB_TRANSFERS; i++) {
				if (usb_transfer_is_free(transfers[i])) {
					transfer = transfers[i];
					break;
				}
			}

			if (transfer)
				break;

			usleep(100);
		}

		/* Reset the transfer length */
		transfer->length = BUFSIZE;

		count = read(STDIN_FILENO, transfer->buffer, transfer->length);
		if (!count)
			break;

		if (count < 0) {
			if (errno == EAGAIN) {
				usleep(100);
				continue;
			}
		}

		transfer->length = count;
		usb_transfer_set_free(transfer, false);
		libusb_submit_transfer(transfer);
	}

	printf("WRITE THREAD FAIL!\n");
	return NULL;
}

static int find_eps(libusb_device_handle *hdl, int *ep_in, int *ep_out)
{
	int ret;
	unsigned int i;
	struct libusb_config_descriptor *desc;
	const struct libusb_interface_descriptor *idesc;
	libusb_device *dev = libusb_get_device(hdl);

	ret = libusb_get_active_config_descriptor(dev, &desc);
	if (ret < 0)
		return ret;

	if (!desc->bNumInterfaces || !desc->interface->num_altsetting)
		return -ENODEV;

	idesc = desc->interface->altsetting;
	if (idesc->bNumEndpoints < 2)
		return -ENXIO;

	for (i = 0; i < idesc->bNumEndpoints; i++) {
		const struct libusb_endpoint_descriptor *ep =
			&idesc->endpoint[i];
		if (ep->bEndpointAddress & LIBUSB_ENDPOINT_IN)
			*ep_in = ep->bEndpointAddress;
		else
			*ep_out = ep->bEndpointAddress;
	}

	return 0;
}

static void usb_data_received_cb(struct libusb_transfer *transfer)
{
	switch (transfer->status) {
	default:
		fprintf(stderr, "USB transfer error: %i\n", transfer->status);
	case LIBUSB_TRANSFER_CANCELLED:
		fprintf(stderr, "Transfer receive cancelled!\n");
	case LIBUSB_TRANSFER_COMPLETED:
		break;
	}

	write(STDOUT_FILENO, transfer->buffer, transfer->actual_length);

	/* Re-append the transfer */
	libusb_submit_transfer(transfer);
}

int main(int argc, char **argv)
{
	unsigned int i;
	unsigned short vid, pid;
	int ret, ep_in, ep_out;
	libusb_context *ctx;
	libusb_device_handle *hdl;
	struct thd_data *thd_data;
	pthread_t thd;
	struct libusb_transfer * transfers[NB_TRANSFERS];

	if (argc < 3) {
		printf("Usage: usbclient <vid> <pid>\n");
		return EXIT_SUCCESS;
	}

	vid = (unsigned short) strtol(argv[1], NULL, 0);
	pid = (unsigned short) strtol(argv[2], NULL, 0);

	ret = libusb_init(&ctx);
	if (ret)
		return EXIT_FAILURE;

	libusb_set_debug(ctx, LIBUSB_LOG_LEVEL_WARNING);
	hdl = libusb_open_device_with_vid_pid(ctx, vid, pid);
	if (!hdl) {
		fprintf(stderr, "Unable to find device 0x%x:0x%x\n", vid, pid);
		goto err_libusb_exit;
	}

	libusb_set_auto_detach_kernel_driver(hdl, 1);

	ret = libusb_claim_interface(hdl, 0);
	if (ret < 0)
		goto err_libusb_close;

	ret = find_eps(hdl, &ep_in, &ep_out);
	if (ret < 0) {
		fprintf(stderr, "Unable to read descriptors: %i\n", ret);
		goto err_libusb_close;
	}

	thd_data = malloc(sizeof(*thd_data));
	if (!thd_data)
		goto err_libusb_close;

	thd_data->hdl = hdl;
	thd_data->ep = ep_out;
	thd_data->thd_stop = false;
	pthread_create(&thd, NULL, write_thd, thd_data);

	for (i = 0; i < NB_TRANSFERS; i++) {
		transfers[i] = usb_alloc_transfer(hdl,
				ep_in, usb_data_received_cb, NULL);
		libusb_submit_transfer(transfers[i]);
	}

	while (!libusb_handle_events(ctx));

	pthread_join(thd, NULL);

	for (i = 0; i < NB_TRANSFERS; i++)
		libusb_free_transfer(transfers[i]);

	free(thd_data);
err_libusb_close:
	libusb_close(hdl);
err_libusb_exit:
	libusb_exit(ctx);
	return EXIT_FAILURE;
}
