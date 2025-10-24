/**
 * Copyright (C) The software Authors. All rights reserved.
 * File Name: src/rdma_server.cpp
 * Author:
 * mail:
 * Created Time: Wed Oct 15 03:42:35 2025
 * Brief:
 */
#include "rdma_common.h"

/* Step 1. Starts an RDMA server by allocating basic connection resources,
 * which are standby resoruce for a server:
 *   * cm_event_channel
 *   * rdma_cm_id (cm_server_id), will bind with addr
 * */
int RdmaServer::start_rdma_server(struct sockaddr_in *server_addr) {
    struct rdma_cm_event *cm_event = NULL;
    int ret = -1;
    /*  Open a channel used to report asynchronous communication event */
    this->cm_event_channel = rdma_create_event_channel();
    if (!this->cm_event_channel) {
        rdma_error("Creating cm event channel failed with errno : (%d)",
                   -errno);
        return -errno;
    }
    debug("RDMA CM event channel is created successfully at %p \n",
          cm_event_channel);
    /* rdma_cm_id is the connection identifier (like socket) which is used
     * to define an RDMA connection.
     */
    ret = rdma_create_id(this->cm_event_channel, &this->cm_server_id, NULL,
                         RDMA_PS_TCP);
    check_ret_and_return(ret, "Creating server cm id failed");
    debug("A RDMA connection id for the server is created \n");
    /* Explicit binding of rdma cm id to the socket credentials */
    ret = rdma_bind_addr(this->cm_server_id, (struct sockaddr *)server_addr);
    check_ret_and_return(ret, "Failed to bind server address");
    debug("Server RDMA CM id is successfully binded \n");
    /* Now we start to listen on the passed IP and port. However unlike
     * normal TCP listen, this is a non-blocking call. When a new client is
     * connected, a new connection management (CM) event is generated on the
     * RDMA CM event channel from where the listening id was created. Here we
     * have only one channel, so it is easy. */
    ret = rdma_listen(this->cm_server_id,
                      8); /* backlog = 8 clients, same as TCP, see man listen*/
    check_ret_and_return(ret, "rdma_listen failed to listen on server address");
    spdlog::info("Server is listening successfully at: {} , port: {} ",
                 inet_ntoa(server_addr->sin_addr),
                 ntohs(server_addr->sin_port));

    return ret;
}

/* When we call this function rdmaServer.cm_client_id must be set to a valid
 * identifier. This is where, we prepare client connection before we accept it.
 * This mainly involve pre-posting a receive buffer to receive client side RDMA
 * credentials
 * Creating client-wise resources for handling communication:
 */

int RdmaServer::wait_for_connect_event() {
    int ret = -1;
    struct rdma_cm_event *cm_event = NULL;
    /* now, we expect a client to connect and generate a
     * RDMA_CM_EVNET_CONNECT_REQUEST We wait (block) on the connection
     * management event channel for the connect event.
     */
    ret = process_rdma_cm_event(this->cm_event_channel,
                                RDMA_CM_EVENT_CONNECT_REQUEST, &cm_event);
    check_ret_and_return(ret, "Failed to get cm event:{}",
                         RDMA_CM_EVENT_CONNECT_REQUEST);
    /* Much like TCP connection, listening returns a new connection identifier
     * for newly connected client. In the case of RDMA, this is stored in id
     * field. For more details: man rdma_get_cm_event
     */
    this->cm_client_id = cm_event->id;
    /* now we acknowledge the event. Acknowledging the event free the resources
     * associated with the event structure. Hence any reference to the event
     * must be made before acknowledgment. Like, we have already saved the
     * client id from "id" field before acknowledging the event.
     */
    ret = rdma_ack_cm_event(cm_event);
    check_ret_and_return(ret, "Failed to ack cm event");
    debug("A new RDMA client connection id is stored at %p\n",
          this->cm_client_id);
    return ret;
}

// Provision a buf to store RdmaBufferAttr from the client side by send/recv;
int RdmaServer::prepare_buf_to_recv_client_meta() {
    int ret = -1;
    if (!this->m_clientCtx.cm_client_id || !this->m_clientCtx.client_qp) {
        rdma_error("Client resources are not properly setup\n");
        return -EINVAL;
    }
    this->clientMeta.Allocate(sizeof(struct RdmaBufferAttr));
    this->clientMeta.Attach(this->m_clientCtx.pd, (IBV_ACCESS_LOCAL_WRITE));
    this->m_clientCtx.client_metadata_attr =
        (struct RdmaBufferAttr *)this->clientMeta.get_buf();

    this->clientMeta.provision_recv_buf(this->m_clientCtx.client_qp);

    /* we prepare the receive buffer in which we will receive the client
     * metadata*/
    return 0;
}

// Pre-posts a receive buffer and accepts an RDMA client connection:
// 1. provision buf for recving.
int RdmaServer::accept_client_connection() {
    struct rdma_conn_param conn_param;
    struct rdma_cm_event *cm_event = NULL;
    struct sockaddr_in remote_sockaddr;
    int ret = -1;

    ret = prepare_buf_to_recv_client_meta();
    check_ret_and_return(ret, "Failed to pre-post the receive buffer");

    /* Now we accept the connection. Recall we have not accepted the connection
     * yet because we have to do lots of resource pre-allocation */
    memset(&conn_param, 0, sizeof(conn_param));
    /* this tell how many outstanding requests can we handle */
    conn_param.initiator_depth =
        3; /* For this exercise, we put a small number here */
    /* This tell how many outstanding requests we expect other side to handle */
    conn_param.responder_resources =
        3; /* For this exercise, we put a small number */
    ret = rdma_accept(this->cm_client_id, &conn_param);
    check_ret_and_return(ret, "Failed to accept the connection");
    /* We expect an RDMA_CM_EVNET_ESTABLISHED to indicate that the RDMA
     * connection has been established and everything is fine on both, server
     * as well as the client sides.
     */
    debug("Going to wait for : RDMA_CM_EVENT_ESTABLISHED event \n");
    ret = process_rdma_cm_event(this->cm_event_channel,
                                RDMA_CM_EVENT_ESTABLISHED, &cm_event);
    check_ret_and_return(ret, "Failed to get the cm event");
    /* We acknowledge the event */
    ret = rdma_ack_cm_event(cm_event);
    check_ret_and_return(ret, "Failed to acknowledge the cm event");
    /* Just FYI: How to extract connection information */
    memcpy(
        &remote_sockaddr /* where to save */,
        rdma_get_peer_addr(this->cm_client_id) /* gives you remote sockaddr */,
        sizeof(struct sockaddr_in) /* max size */);
    printf("A new connection is accepted from %s \n",
           inet_ntoa(remote_sockaddr.sin_addr));
    return ret;
}

/* This function sends server side buffer metadata to the connected client */
int RdmaServer::send_server_metadata_to_client() {
    struct ibv_wc wc;
    int ret = -1;
    /* Now, we first wait for the client to start the communication by
     * sending the server its metadata info. The server does not use it
     * in our example. We will receive a work completion notification for
     * our pre-posted receive request.
     */
    ret = process_work_completion_events(
        this->m_clientCtx.io_completion_channel, &wc, 1);
    if (ret != 1) {
        rdma_error("Failed to receive , ret = %d \n", ret);
        return ret;
    }
    /* if all good, then we should have client's buffer information, lets see */
    /* We need to setup requested memory buffer. This is where the client will
     * do RDMA READs and WRITEs. */
    printf("Client side buffer information is received...\n");
    show_rdma_buffer_attr(this->m_clientCtx.client_metadata_attr);
    printf("The client has requested buffer length of : %u bytes \n",
           this->m_clientCtx.client_metadata_attr->length);

    this->serverBuffer.Allocate(this->m_clientCtx.client_metadata_attr->length);
    this->serverBuffer.Attach(this->m_clientCtx.pd,
                              (enum ibv_access_flags)(IBV_ACCESS_LOCAL_WRITE |
                                                      IBV_ACCESS_REMOTE_READ |
                                                      IBV_ACCESS_REMOTE_WRITE));
    /* This buffer is used to transmit information about the above
     * buffer to the client. So this contains the metadata about the server
     * buffer. Hence this is called metadata buffer. Since this is already
     * on allocated, we just register it.
     * We need to prepare a send I/O operation that will tell the
     * client the address of the server buffer.
     */
    // 2. Prepare the SimpleBuffer for metadata of remote mr
    // 2.1 Allocate memory and init it with client metadata info
    this->serverMeta.Allocate(sizeof(struct RdmaBufferAttr));

    // Cache the local-generated values and SENDING to the serverside.
    RdmaBufferAttr *server_metadata_attr =
        (struct RdmaBufferAttr *)(this->serverMeta.get_buf());

    server_metadata_attr->address = (uint64_t)this->serverBuffer.get_mr()->addr;
    server_metadata_attr->length = this->serverBuffer.get_mr()->length;
    server_metadata_attr->stag.local_stag = this->serverBuffer.get_mr()->lkey;
    // Init the mr to tell the server side the necessary keys.
    this->serverMeta.Attach(this->m_clientCtx.pd, IBV_ACCESS_LOCAL_WRITE);
    // Sending the request for exchanging the metadata
    this->serverMeta.remote_msg(this->m_clientCtx.client_qp, IBV_WR_SEND);

    /* We check for completion notification */
    ret = process_work_completion_events(
        this->m_clientCtx.io_completion_channel, &wc, 1);
    if (ret != 1) {
        rdma_error("Failed to send server metadata, ret = %d \n", ret);
        return ret;
    }
    debug("Local buffer metadata has been sent to the client \n");
    return 0;
}

/* This is server side logic. Server passively waits for the client to call
 * rdma_disconnect() and then it will clean up its resources
 *   Creation order:
 * 1. Protection Domains (PD)
 * 2. Memory Buffers
 * 3. Completion Queues (CQ)
 * 4. Queue Pair (QP)
 *   Deletion order:
 * 1. Queue Pair (QP)
 * 2. cm_client_id
 * 3. Completion Queues (CQ)
 * 4. io_completion_channel
 * 5. mr ? (multiple mrs)
 * 6. pd
 * 7. cm_server_id
 * 8. cm_event_channel
 * */
int RdmaServer::disconnect_and_cleanup() {
    struct rdma_cm_event *cm_event = NULL;
    int ret = -1;
    /* Now we wait for the client to send us disconnect event */
    debug("Waiting for cm event: RDMA_CM_EVENT_DISCONNECTED\n");
    ret = process_rdma_cm_event(this->cm_event_channel,
                                RDMA_CM_EVENT_DISCONNECTED, &cm_event);
    check_ret_and_return(ret, "Failed to get disconnect event");
    /* We acknowledge the event */
    ret = rdma_ack_cm_event(cm_event);
    check_ret_and_return(ret, "Failed to acknowledge the cm event");
    printf("A disconnect event is received from the client...\n");

    /* Destroy memory buffers */
    spdlog::info("Mr data before disconnectted:{}",
                 (char *)(this->serverBuffer.get_mr()->addr));
    this->serverBuffer.DeAttach();
    this->serverBuffer.DeAllocate();
    // rdma_buffer_deregister(this->server_metadata_mr);
    this->serverMeta.DeAttach();
    this->serverMeta.DeAllocate();
    this->clientMeta.DeAttach();
    // rdma_buffer_deregister(this->client_metadata_mr);

    this->m_clientCtx.CleanupCtx();

    return 0;
}

int RdmaServer::server_cleanup() {
    // struct rdma_cm_event *cm_event = NULL;
    int ret = -1;
    // Destroy protection domain
    ret = ibv_dealloc_pd(this->m_clientCtx.pd);
    check_ret_and_error(ret,
                        "Failed to destroy client protection domain cleanly");
    // Destroy rdma server id
    ret = rdma_destroy_id(this->cm_server_id);
    check_ret_and_error(ret, "Failed to destroy server id cleanly");

    rdma_destroy_event_channel(this->cm_event_channel);
    printf("Server shut-down is complete \n");
    return 0;
}
