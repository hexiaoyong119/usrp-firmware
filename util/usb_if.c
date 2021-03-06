/*
 * Copyright 2018 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "usb_if.h"

/* Return 0 on error, since it's never gonna be EP 0 */
static int find_endpoint(const struct libusb_interface_descriptor *iface,
			 uint16_t subclass,
			 uint16_t protocol,
			 struct usb_endpoint *uep)
{
	const struct libusb_endpoint_descriptor *ep;

	if (iface->bInterfaceClass == 255 &&
	    iface->bInterfaceSubClass == subclass &&
	    iface->bInterfaceProtocol == protocol &&
	    iface->bNumEndpoints) {
		ep = &iface->endpoint[0];
		uep->ep_num = ep->bEndpointAddress & 0x7f;
		uep->chunk_len = ep->wMaxPacketSize;
		return 1;
	}

	return 0;
}

/* Return -1 on error */
static int find_interface(uint16_t subclass,
			  uint16_t protocol,
			  struct usb_endpoint *uep)
{
	int iface_num = -1;
	int r, i, j;
	struct libusb_device *dev;
	struct libusb_config_descriptor *conf = 0;
	const struct libusb_interface *iface0;
	const struct libusb_interface_descriptor *iface;

	dev = libusb_get_device(uep->devh);
	r = libusb_get_active_config_descriptor(dev, &conf);
	if (r < 0) {
		USB_ERROR("libusb_get_active_config_descriptor", r);
		goto out;
	}

	for (i = 0; i < conf->bNumInterfaces; i++) {
		iface0 = &conf->interface[i];
		for (j = 0; j < iface0->num_altsetting; j++) {
			iface = &iface0->altsetting[j];
			if (find_endpoint(iface, subclass, protocol, uep)) {
				iface_num = i;
				goto out;
			}
		}
	}

out:
	libusb_free_config_descriptor(conf);
	return iface_num;
}

int usb_findit(uint16_t vid, uint16_t pid, uint16_t subclass,
	       uint16_t protocol, struct usb_endpoint *uep)
{
	int iface_num, r;

	memset(uep, 0, sizeof(*uep));

	r = libusb_init(NULL);
	if (r < 0) {
		USB_ERROR("libusb_init", r);
		return -1;
	}

	printf("open_device %04x:%04x\n", vid, pid);
	/* NOTE: This doesn't handle multiple matches! */
	uep->devh = libusb_open_device_with_vid_pid(NULL, vid, pid);
	if (!uep->devh) {
		fprintf(stderr, "Can't find device\n");
		return -1;
	}

	iface_num = find_interface(subclass, protocol, uep);
	if (iface_num < 0) {
		fprintf(stderr, "USB FW update not supported by that device\n");
		usb_shut_down(uep);
	}
	if (!uep->chunk_len) {
		fprintf(stderr, "wMaxPacketSize isn't valid\n");
		usb_shut_down(uep);
	}

	printf("found interface %d endpoint %d, chunk_len %d\n",
	       iface_num, uep->ep_num, uep->chunk_len);

	libusb_set_auto_detach_kernel_driver(uep->devh, 1);
	r = libusb_claim_interface(uep->devh, iface_num);
	if (r < 0) {
		USB_ERROR("libusb_claim_interface", r);
		usb_shut_down(uep);
	}

	printf("READY\n-------\n");
	return 0;
}

int usb_trx(struct usb_endpoint *uep, void *outbuf, int outlen,
	    void *inbuf, int inlen, int allow_less, size_t *rxed_count)
{

	int r, actual;

	/* Send data out */
	if (outbuf && outlen) {
		actual = 0;
		r = libusb_bulk_transfer(uep->devh, uep->ep_num,
					 outbuf, outlen,
					 &actual, 1000);
		if (r < 0) {
			USB_ERROR("libusb_bulk_transfer", r);
			return -1;
		}
		if (actual != outlen) {
			fprintf(stderr, "%s:%d, only sent %d/%d bytes\n",
				__FILE__, __LINE__, actual, outlen);
			usb_shut_down(uep);
		}
	}

	/* Read reply back */
	if (inbuf && inlen) {

		actual = 0;
		r = libusb_bulk_transfer(uep->devh, uep->ep_num | 0x80,
					 inbuf, inlen,
					 &actual, 1000);
		if (r < 0) {
			USB_ERROR("libusb_bulk_transfer", r);
			return -1;
		}
		if ((actual != inlen) && !allow_less) {
			fprintf(stderr, "%s:%d, only received %d/%d bytes\n",
				__FILE__, __LINE__, actual, inlen);
			usb_shut_down(uep);
		}

		if (rxed_count)
			*rxed_count = actual;
	}

	return 0;
}

void usb_shut_down(struct usb_endpoint *uep)
{
	libusb_close(uep->devh);
	libusb_exit(NULL);
}
