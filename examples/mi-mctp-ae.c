// SPDX-License-Identifier: LGPL-2.1-or-later
/**
 * This file is part of libnvme.
 *
 * Authors: Chuck Horkin <chorkin@microsoft.com>
 */

/**
 * mi-mctp-ae: open a MI connection over MCTP, supporting asynchronous event messages
 */

#include <assert.h>
#include <ctype.h>
#include <err.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <unistd.h> // for usleep

#include <libnvme-mi.h>
#include <poll.h>

#include <ccan/array_size/array_size.h>
#include <ccan/endian/endian.h>

// Function to print the byte array
static void print_byte_array(void *data, size_t len)
{
	uint8_t *byte_data = (uint8_t *)data;

	for (size_t i = 0; i < len; ++i)
		printf("%02X ", byte_data[i]);
	printf("\n");
}

static void print_event_info(struct nvme_mi_event *event)
{
	printf("aeoi: %02X\n", event->aeoi);
	printf("aeocidi: %04X\n", event->aeocidi);
	printf("aessi: %02X\n", event->aessi);

	printf("specific_info: ");
	if (event->spec_info_len && event->spec_info)
		print_byte_array(event->spec_info, event->spec_info_len);
	else
		printf("EMPTY\n");

	printf("vendor_specific_info: ");
	if (event->vend_spec_info_len && event->vend_spec_info)
		print_byte_array(event->vend_spec_info, event->vend_spec_info_len);
	else
		printf("EMPTY\n");
}

enum handler_next_action aem_handler(nvme_mi_ep_t ep, size_t num_events, struct nvme_mi_aem_ctx *aem_ctx, void *userdata)
{
	uint32_t *count = (uint32_t *) userdata;
	*count = *count+1;

	printf("Received notification #%ls\n with %ld events", count, num_events);
	for (int i = 0; i < num_events; i++) {
		struct nvme_mi_event *event = nvme_mi_aem_get_next_event(aem_ctx);

		if (event == NULL)
			printf("Unexpected NULL event\n");
		else {
			printf("Event:\n");
			print_event_info(event);
			printf("\n");
		}
	}

	return Ack;
}

int main(int argc, char **argv)
{
	nvme_root_t root;
	nvme_mi_ep_t ep;
	bool usage = true;
	uint8_t eid = 0;
	int rc = 0, net = 0;
	struct nvme_mi_aem_callbacks aem_cb_info = {0};
	struct nvme_mi_aem_ctx *aem_ctx = NULL;
	uint32_t notification_counter = 0;

	if (argc == 3) {
		usage = false;
		net = atoi(argv[1]);
		eid = atoi(argv[2]) & 0xff;
		argv += 2;
		argc -= 2;
	}

	if (usage) {
		fprintf(stderr,
			"usage: %s <net> <eid>\n",
			argv[0]);
		return EXIT_FAILURE;
	}

	//Otherwise, let's just jump into the loop
	printf("To exit, terminate with CTRL+C\n");

	root = nvme_mi_create_root(stderr, DEFAULT_LOGLEVEL);
	if (!root)
		err(EXIT_FAILURE, "can't create NVMe root");

	ep = nvme_mi_open_mctp(root, net, eid);
	if (!ep)
		errx(EXIT_FAILURE, "can't open MCTP endpoint %d:%d", net, eid);

	aem_cb_info.aem_handler = aem_handler;
	aem_cb_info.enabled[0] = true;

	rc = nvme_mi_enable_aem(ep, (const struct nvme_mi_aem_callbacks *)&aem_cb_info, &notification_counter, &aem_ctx);
	if (rc)
		errx(EXIT_FAILURE, "Can't enable aem:%d", rc);

	struct pollfd fds;

	rc = nvme_mi_get_pollfd(aem_ctx, &fds);
	if (rc)
		errx(EXIT_FAILURE, "Can't get pollfd:%d", rc);

	while (1) {
		int timeout = 1000; // Timeout in milliseconds ( seconds)

		rc = poll(&fds, 1, timeout);

		if (rc == -1) {
			perror("poll");
			break;
		} else if (rc == 0) {
			//printf("No data within %d milliseconds.\n", timeout);
		} else {
			//Time to do the work
			rc = nvme_mi_aem_process(aem_ctx);
			if (rc) {
				errx(EXIT_FAILURE, "nvme_mi_aem_process failed with:%d", rc);
				return rc;
			}
		}
	}

	//Cleanup
	nvme_mi_disable_aem(aem_ctx);
	nvme_mi_close(ep);
	nvme_mi_free_root(root);

	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}


