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

#include <ccan/array_size/array_size.h>
#include <ccan/endian/endian.h>

enum ParsingResult{
	Error = -1,
	Ack = 0,
	NoError = 0,
	Data = 1
};

static enum ParsingResult validate_occ_list_header(struct nvme_mi_ae_occ_list_hdr* occ_header, size_t len)
{
	//Make sure header fields have valid data
	if(len < sizeof(*occ_header))
	{
		return Error;
	}
	else if(occ_header->aelver != 0 || occ_header->aeolhl != sizeof(*occ_header))
	{
		//Make sure header is the right version and length
		return Error;
	}
	else if(occ_header->aeolli.aeoltl > len)
	{
		//Full length is bigger than the data that was received
		return Error;
	}

	return NoError;
}

static enum ParsingResult validate_update_rdy_event(struct nvme_mi_ae_occ_list_hdr* occ_header, size_t len, uint8_t* ready_state)
{
	//Assumption is validate_occ_list_header has already been called
	if(occ_header->numaeo == 0)
	{
		//This is just an ack
		return Ack;
	}
	else if(occ_header->numaeo != 1)
	{
		return Error;//This means we're getting more events than we would expect
	}
		
	struct nvme_mi_ae_occ_data* occ_data = (struct nvme_mi_ae_occ_data*)((uint8_t*)occ_header + sizeof(*occ_header));
	if(occ_data->aelhlen != sizeof(*occ_data) || occ_data->aeosil != sizeof(ready_state))
	{
		return Error;
	}
	else if(occ_data->aeoui.aeoi != 0)
	{
		//This isn't associated with AE #0 (Ready)
		return Error;
	}

	*ready_state = *(uint8_t*)((uint8_t*)occ_data + sizeof(*occ_data));//Pull specific info to udpate state

	return Data;
}

//Return 1 if just an ack, Return 0 if ready_state updated, Return -1 if error
static enum ParsingResult parse_aem_update_rdy(struct nvme_mi_ae_occ_list_hdr* occ_header, size_t len, uint8_t* ready_state)
{
	if(NoError == validate_occ_list_header(occ_header, len))//Probably want to push as much of this into libnvme as possible
	{
		//Provide remaining length (beyond original header) plus OCC data for validation
		enum ParsingResult result = validate_update_rdy_event(occ_header, len, ready_state);
		if(result == Error)
		{
			fprintf(stderr, "Error validating AEM Occurrence Data.\n");
		}
		
		return result;
	}
	else
	{
		fprintf(stderr, "Error validating AEM Occurrence List Header.\n");
		return Error;
	}
}

int main(int argc, char **argv)
{
	nvme_root_t root;
	nvme_mi_ep_t ep;
	bool usage = true;
	uint8_t eid = 0;
	int rc = 0, net = 0;
	enum ParsingResult pr = NoError;

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

	uint8_t cmd_buffer[4096] = {0};
	struct ae_enable_list_t* enable_list = (struct ae_enable_list_t*)cmd_buffer;
	struct ae_enable_list_header_t* header = &enable_list->hdr;//Just for some shorthand below
	
	header->aeelhl = sizeof(struct ae_enable_list_header_t);
	header->numaee = 1;//List only contains one enable item
	header->aeelver = 0;
	header->aeetl = header->aeelhl + ( header->numaee * sizeof(struct ae_enable_item_t ));

	struct ae_enable_item_t * item = (struct ae_enable_item_t*)((uint8_t*)header + sizeof(struct ae_enable_list_header_t));
	item->aeel = sizeof(struct ae_enable_item_t);
	item->aeei.aee = 1;//Enabled
	item->aeei.aeeid = 0; //RDY Info AE

	uint8_t response_buffer[4096] ={0};
	size_t response_len = sizeof(response_buffer);
	struct nvme_mi_ae_occ_list_hdr* response= (struct nvme_mi_ae_occ_list_hdr*)response_buffer;

	uint8_t last_rdy_state = 0;

	//Subscribe to notifications with AEM Sync (Configuration Set AE with non-zero number of entries)
	rc = nvme_mi_mi_config_set_async_event(ep,
										   true,
										   true,
										   true,
										   2 /*Delay Interval (Seconds)*/,
										   0 /*No Retry Delay*/,
										   enable_list,
										   header->aeetl,
										   response,
										   &response_len);

	if(rc != 0)
	{
		fprintf(stderr, "Error configuring AEM Sync: %d\n", rc);
	}
	else
	{
		pr = validate_update_rdy_event(response, response_len, &last_rdy_state);
		if(pr == Ack)
		{
			printf("Unexpected empty OCC list\n");
			rc = -1;
		}
	}

	if(rc == 0)
	{
		printf("Starting Loop.  Exit by killing process (CTRL+C)\n");
		memset(response_buffer, 0, sizeof(response_buffer));
		while(1)
		{
			struct nvme_mi_aem_msg* aem = (struct nvme_mi_aem_msg*)response_buffer;
			response_len = sizeof(response_buffer);
			memset(response_buffer, 0, sizeof(response_buffer));

			rc = nvme_mi_get_async_message(ep, aem, &response_len);
			if(rc == -1 && errno == EWOULDBLOCK)
			{
				//Seems no data available yet
				continue;
			}
			else if(rc != 0)
			{
				fprintf(stderr, "Error from nvme_mi_get_async_message: %d\n", rc);
				break;
			}
			else
			{
				printf("AEM received\n");

				pr = parse_aem_update_rdy(&aem->occ_list_hdr, response_len, &last_rdy_state);
				//Note: While this example has an AE with specific info, this is not required
				//for all AEs.  See the MCTP-MI spec.
				if(Data == pr )
				{
					printf("RDY AE (01h).  New state is %d\n", last_rdy_state);
				}
				else
				{
					//An Ack is not expected here, nor is an ERror
					fprintf(stderr, "Error parsing AEM\n");
					rc = -1;
					break;
				}

				//Send Ack to put endpoint state machine back to armed state, ackgnowledging the AE message
				response_len = sizeof(response_buffer);
				rc = aem_ack(ep, response, &response_len);	

				if(rc == 0)
				{
					//AEM Ack comes with any events that happened since the AEM was sent
					pr = parse_aem_update_rdy(response, response_len, &last_rdy_state);
					if(Data == pr)
					{
						printf("RDY AE (01h).  New state is %d\n", last_rdy_state);
					}
					else if(NoError == pr)
					{
						//It's fine not to have a second event here
					}
					else
					{
						fprintf(stderr, "Error parsing ack response\n");
						rc = -1;
						break;
					}
				}	
			}
			usleep(10000); // Sleep for 10 milliseconds
		}
	}
	else
	{
		fprintf(stderr, "Error with AEM Sync response: %d.\n", rc);
	}

	//Cleanup
	nvme_mi_close(ep);
	nvme_mi_free_root(root);
	
	return rc ? EXIT_FAILURE : EXIT_SUCCESS;
}


