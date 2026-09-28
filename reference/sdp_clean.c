#include <bluetooth/bluetooth.h>
#include <bluetooth/sdp.h>
#include <bluetooth/sdp_lib.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static sdp_session_t *session = NULL;
static uint32_t aa_record_handle = 0;

static void cleanup_and_exit(int sig) {
    (void)sig;
    if (session) {
        if (aa_record_handle) {
            sdp_device_record_unregister_binary(session, NULL, aa_record_handle);
        }
        sdp_close(session);
    }
    exit(0);
}

static int remove_core_records(sdp_session_t *sess, bdaddr_t *local) {
    int result = 0;
    for (uint32_t h = 0x10000; h <= 0x10003; h++) {
        if (sdp_device_record_unregister_binary(sess, local, h) < 0) {
            fprintf(stderr, "Warning: failed to remove core record 0x%04x\n", h);
            result = -1;
        } else {
            fprintf(stderr, "Removed core SDP record 0x%04x\n", h);
        }
    }
    return result;
}

static sdp_record_t *create_aa_record(void) {
    sdp_record_t *record = sdp_record_alloc();
    if (!record) {
        return NULL;
    }

    uint128_t uuid128 = {{
        0x4d, 0xe1, 0x7a, 0x00, 0x52, 0xcb, 0x11, 0xe6,
        0xbd, 0xf4, 0x08, 0x00, 0x20, 0x0c, 0x9a, 0x66
    }};
    uuid_t aa_uuid;
    sdp_uuid128_create(&aa_uuid, &uuid128);

    sdp_list_t *service_classes = sdp_list_append(NULL, &aa_uuid);
    sdp_set_service_classes(record, service_classes);
    sdp_list_free(service_classes, NULL);

    uuid_t l2cap_uuid, rfcomm_uuid;
    sdp_uuid16_create(&l2cap_uuid, L2CAP_UUID);
    sdp_uuid16_create(&rfcomm_uuid, RFCOMM_UUID);

    sdp_list_t *l2cap_proto = sdp_list_append(NULL, &l2cap_uuid);
    sdp_list_t *rfcomm_proto = sdp_list_append(NULL, &rfcomm_uuid);
    uint8_t channel = 8;
    sdp_data_t *channel_data = sdp_data_alloc(SDP_UINT8, &channel);
    rfcomm_proto = sdp_list_append(rfcomm_proto, channel_data);

    sdp_list_t *apseq = sdp_list_append(NULL, l2cap_proto);
    apseq = sdp_list_append(apseq, rfcomm_proto);

    sdp_list_t *aproto = sdp_list_append(NULL, apseq);
    sdp_set_access_protos(record, aproto);

    uuid_t root_uuid;
    sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
    sdp_list_t *browse_list = sdp_list_append(NULL, &root_uuid);
    sdp_set_browse_groups(record, browse_list);
    sdp_list_free(browse_list, NULL);

    sdp_set_info_attr(record, "Android Auto Wireless", NULL, NULL);

    return record;
}

int main(void) {
    bdaddr_t any = {{0}};
    bdaddr_t local = {{0, 0, 0, 0xff, 0xff, 0xff}};

    signal(SIGINT, cleanup_and_exit);
    signal(SIGTERM, cleanup_and_exit);

    session = sdp_connect(&any, &local, SDP_RETRY_IF_BUSY);
    if (!session) {
        fprintf(stderr, "Failed to connect to SDP server. Is bluetoothd running with --compat?\n");
        return 1;
    }

    fprintf(stderr, "Connected to SDP server\n");

    remove_core_records(session, &local);

    sdp_record_t *aa_record = create_aa_record();
    if (!aa_record) {
        fprintf(stderr, "Failed to create AA SDP record\n");
        sdp_close(session);
        return 1;
    }

    if (sdp_device_record_register(session, &local, aa_record, 0) < 0) {
        fprintf(stderr, "Failed to register AA SDP record\n");
        sdp_record_free(aa_record);
        sdp_close(session);
        return 1;
    }

    aa_record_handle = aa_record->handle;
    fprintf(stderr, "Registered AA SDP record with handle 0x%04x\n", aa_record_handle);
    fprintf(stderr, "SDP record active. Press Ctrl+C to exit.\n");

    sdp_record_free(aa_record);

    while (1) {
        pause();
    }

    return 0;
}