#ifndef RDMA_SERVER_H
#define RDMA_SERVER_H

#include "rdma_common.h"

class RdmaServer {
public:
    struct rdma_cm_id *cm_client_id;

private:
    struct rdma_event_channel *cm_event_channel;
    struct rdma_cm_id *cm_server_id;

    SimpleBuffer serverBuffer;

    SimpleBuffer clientMeta;
    SimpleBuffer serverMeta;

public:
    ClientCtx m_clientCtx;

public:
    RdmaServer()
        : cm_event_channel(NULL), cm_server_id(NULL), cm_client_id(NULL) {}

    int start_rdma_server(struct sockaddr_in *server_addr);
    // int setup_client_resources();
    int accept_client_connection();
    int send_server_metadata_to_client();
    int disconnect_and_cleanup();
    int server_cleanup();

    // Block: waiting for connect event on connection management.
    int wait_for_connect_event();

private:
    // Provision a buf to store RdmaBufferAttr from the client side by
    // send/recv;
    int prepare_buf_to_recv_client_meta();
};

#endif // RDMA_SERVER_H
