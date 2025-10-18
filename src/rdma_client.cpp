/**
 * Copyright (C) The software Authors. All rights reserved.
 * File Name: rdma_client.cpp
 * Author:
 * mail:
 * Created Time: Wed Oct 15 03:37:05 2025
 * Brief:
 */
#include "rdma_common.h"

/* Source and Destination buffers, where RDMA operations source and sink */
// char *src = NULL, *dst = NULL;

/* This is our testing function */
int check_src_dst(uint8_t *src, uint8_t *dst) {
    return memcmp((void *)src, (void *)dst, strlen((const char *)src));
}

/* Step 1: This function prepares client side connection resources for
 * an RDMA connection (no interaction to the remote server):
 * 1) create event_channel;
 * 2) create client_id on top of event_channel;
 * 3) resolve the address (client_id is optional).
 * 4) block for handling resolve event.
 * 5) establish connection: resolve an RDMA route to destination address
 * 6) create the resource:
 *    * pd
 *    * io_completion_channel
 *    * cq of client
 *    * qp of client
 * */
int RdmaClient::client_prepare_connection(struct sockaddr_in *s_addr) {
    struct rdma_cm_event *cm_event = NULL;
    int ret = -1;
    /*  Open a channel used to report asynchronous communication event */
    this->cm_event_channel = rdma_create_event_channel();
    if (!this->cm_event_channel) {
        rdma_error("Creating cm event channel failed, errno: %d \n", -errno);
        return -errno;
    }
    debug("RDMA CM event channel is created at : %p \n",
          this->cm_event_channel);
    /* rdma_cm_id is the connection identifier (like socket) which is used
     * to define an RDMA connection.
     */
    ret = rdma_create_id(this->cm_event_channel, &this->cm_client_id, NULL,
                         RDMA_PS_TCP);
    if (ret) {
        rdma_error("Creating cm id failed with errno: %d \n", -errno);
        return -errno;
    }
    /* Resolve destination and optional source addresses from IP addresses  to
     * an RDMA address.  If successful, the specified rdma_cm_id will be bound
     * to a local device. */
    ret = rdma_resolve_addr(this->cm_client_id, NULL, (struct sockaddr *)s_addr,
                            2000);
    if (ret) {
        rdma_error("Failed to resolve address, errno: %d \n", -errno);
        return -errno;
    }
    debug("waiting for cm event: RDMA_CM_EVENT_ADDR_RESOLVED\n");
    ret = process_rdma_cm_event(this->cm_event_channel,
                                RDMA_CM_EVENT_ADDR_RESOLVED, &cm_event);
    if (ret) {
        rdma_error("Failed to receive a valid event, ret = %d \n", ret);
        return ret;
    }
    /* we ack the event */
    ret = rdma_ack_cm_event(cm_event);
    if (ret) {
        rdma_error("Failed to acknowledge the CM event, errno: %d\n", -errno);
        return -errno;
    }
    debug("RDMA address is resolved \n");

    /* Resolves an RDMA route to the destination address in order to
     * establish a connection */
    ret = rdma_resolve_route(this->cm_client_id, 2000);
    if (ret) {
        rdma_error("Failed to resolve route, erno: %d \n", -errno);
        return -errno;
    }
    debug("waiting for cm event: RDMA_CM_EVENT_ROUTE_RESOLVED\n");
    ret = process_rdma_cm_event(this->cm_event_channel,
                                RDMA_CM_EVENT_ROUTE_RESOLVED, &cm_event);
    if (ret) {
        rdma_error("Failed to receive a valid event, ret = %d \n", ret);
        return ret;
    }
    /* we ack the event */
    ret = rdma_ack_cm_event(cm_event);
    if (ret) {
        rdma_error("Failed to acknowledge the CM event, errno: %d \n", -errno);
        return -errno;
    }
    printf("Trying to connect to server at : %s port: %d \n",
           inet_ntoa(s_addr->sin_addr), ntohs(s_addr->sin_port));
    /* Protection Domain (PD) is similar to a "process abstraction"
     * in the operating system. All resources are tied to a particular PD.
     * And accessing recourses across PD will result in a protection fault.
     */
    this->pd = ibv_alloc_pd(this->cm_client_id->verbs);
    if (!this->pd) {
        rdma_error("Failed to alloc pd, errno: %d \n", -errno);
        return -errno;
    }
    debug("pd allocated at %p \n", this->pd);
    /* Now we need a completion channel, were the I/O completion
     * notifications are sent. Remember, this is different from connection
     * management (CM) event notifications.
     * A completion channel is also tied to an RDMA device, hence we will
     * use this->cm_client_id->verbs.
     */
    this->io_completion_channel =
        ibv_create_comp_channel(this->cm_client_id->verbs);
    if (!this->io_completion_channel) {
        rdma_error("Failed to create IO completion event channel, errno: %d\n",
                   -errno);
        return -errno;
    }
    debug("completion event channel created at : %p \n",
          this->io_completion_channel);
    /* Now we create a completion queue (CQ) where actual I/O
     * completion metadata is placed. The metadata is packed into a structure
     * called struct ibv_wc (wc = work completion). ibv_wc has detailed
     * information about the work completion. An I/O request in RDMA world
     * is called "work" ;)
     */
    this->client_cq = ibv_create_cq(
        this->cm_client_id->verbs /* which device*/,
        CQ_CAPACITY /* maximum capacity*/,
        NULL /* user context, not used here */,
        this->io_completion_channel /* which IO completion channel */,
        0 /* signaling vector, not used here*/);
    if (!this->client_cq) {
        rdma_error("Failed to create CQ, errno: %d \n", -errno);
        return -errno;
    }
    debug("CQ created at %p with %d elements \n", this->client_cq,
          this->client_cq->cqe);
    ret = ibv_req_notify_cq(this->client_cq, 0);
    if (ret) {
        rdma_error("Failed to request notifications, errno: %d\n", -errno);
        return -errno;
    }
    /* Now the last step, set up the queue pair (send, recv) queues and their
     * capacity. The capacity here is define statically but this can be probed
     * from the device. We just use a small number as defined in rdma_common.h
     */
    bzero(&this->qp_init_attr, sizeof this->qp_init_attr);
    this->qp_init_attr.cap.max_recv_sge =
        MAX_SGE; /* Maximum SGE per receive posting */
    this->qp_init_attr.cap.max_recv_wr =
        MAX_WR; /* Maximum receive posting capacity */
    this->qp_init_attr.cap.max_send_sge =
        MAX_SGE; /* Maximum SGE per send posting */
    this->qp_init_attr.cap.max_send_wr =
        MAX_WR; /* Maximum send posting capacity */
    this->qp_init_attr.qp_type =
        IBV_QPT_RC; /* QP type, RC = Reliable connection */
    /* We use same completion queue, but one can use different queues */
    this->qp_init_attr.recv_cq =
        this->client_cq; /* Where should I notify for receive completion
                            operations */
    this->qp_init_attr.send_cq =
        this->client_cq; /* Where should I notify for send completion operations
                          */
    /*Lets create a QP */
    ret = rdma_create_qp(this->cm_client_id /* which connection id */,
                         this->pd /* which protection domain*/,
                         &this->qp_init_attr /* Initial attributes */);
    if (ret) {
        rdma_error("Failed to create QP, errno: %d \n", -errno);
        return -errno;
    }
    this->client_qp = this->cm_client_id->qp;
    debug("QP created at %p \n", this->client_qp);
    return 0;
}

/* Step 2: Pre-posts a receive buffer before calling rdma_connect ()
 * 1) register MR for storing server_metadata (Data structure:
 * server_metadata_attr)
 * */
int RdmaClient::client_pre_post_recv_buffer() {
    int ret = -1;
    this->server_metadata_mr = rdma_buffer_register(
        this->pd, &server_metadata_attr, sizeof(server_metadata_attr),
        (IBV_ACCESS_LOCAL_WRITE));
    if (!this->server_metadata_mr) {
        rdma_error("Failed to setup the server metadata mr , -ENOMEM\n");
        return -ENOMEM;
    }
    this->server_recv_sge.addr = (uint64_t)this->server_metadata_mr->addr;
    this->server_recv_sge.length = (uint32_t)this->server_metadata_mr->length;
    this->server_recv_sge.lkey = (uint32_t)this->server_metadata_mr->lkey;
    /* now we link it to the request */
    bzero(&this->server_recv_wr, sizeof(this->server_recv_wr));
    this->server_recv_wr.sg_list = &this->server_recv_sge;
    this->server_recv_wr.num_sge = 1;
    ret = ibv_post_recv(this->client_qp /* which QP */,
                        &this->server_recv_wr /* receive work request*/,
                        &this->bad_server_recv_wr /* error WRs */);
    if (ret) {
        rdma_error("Failed to pre-post the receive buffer, errno: %d \n", ret);
        return ret;
    }
    debug("Receive buffer pre-posting is successful \n");
    return 0;
}

/* Connects to the RDMA server */
int RdmaClient::client_connect_to_server() {
    struct rdma_conn_param conn_param;
    struct rdma_cm_event *cm_event = NULL;
    int ret = -1;
    bzero(&conn_param, sizeof(conn_param));
    conn_param.initiator_depth = 3;
    conn_param.responder_resources = 3;
    conn_param.retry_count = 3;  // if fail, then how many times to retry
    ret = rdma_connect(this->cm_client_id, &conn_param);
    if (ret) {
        rdma_error("Failed to connect to remote host , errno: %d\n", -errno);
        return -errno;
    }
    debug("waiting for cm event: RDMA_CM_EVENT_ESTABLISHED\n");
    ret = process_rdma_cm_event(this->cm_event_channel,
                                RDMA_CM_EVENT_ESTABLISHED, &cm_event);
    if (ret) {
        rdma_error("Failed to get cm event, ret = %d \n", ret);
        return ret;
    }
    ret = rdma_ack_cm_event(cm_event);
    if (ret) {
        rdma_error("Failed to acknowledge cm event, errno: %d\n", -errno);
        return -errno;
    }
    printf("The client is connected successfully \n");
    return 0;
}

/* Exchange buffer metadata with the server. The client sends its, and then
 * receives from the server. The client-side metadata on the server is _not_
 * used because this program is client driven. But it shown here how to do it
 * for the illustration purposes
 */
int RdmaClient::client_xchange_metadata_with_server(SimpleBuffer *pBuf) {
    struct ibv_wc wc[2];
    int ret = -1;
    int flags = (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ |
                 IBV_ACCESS_REMOTE_WRITE);
    /*
    this->client_write_mr = rdma_buffer_register(
        this->pd, pBuf->src, strlen(pBuf->src), (ibv_access_flags)flags);
    if (!this->client_write_mr) {
        rdma_error("Failed to register the first buffer, ret = %d \n", ret);
        return ret;
    }
    */
    this->recvReq.Attach(this->pd, flags);
    /* we prepare metadata for the first buffer */
    // client_metadata_attr.address = (uint64_t)this->client_write_mr->addr;
    // client_metadata_attr.length = this->client_write_mr->length;
    // client_metadata_attr.stag.local_stag = this->client_write_mr->lkey;
    client_metadata_attr.address = (uint64_t)this->recvReq.get_mr()->addr;
    client_metadata_attr.length = this->recvReq.get_mr()->length;
    client_metadata_attr.stag.local_stag = this->recvReq.get_mr()->lkey;
    /* now we register the metadata memory */
    this->client_metadata_mr = rdma_buffer_register(
        this->pd, &client_metadata_attr, sizeof(client_metadata_attr),
        IBV_ACCESS_LOCAL_WRITE);
    if (!this->client_metadata_mr) {
        rdma_error("Failed to register the client metadata buffer, ret = %d \n",
                   ret);
        return ret;
    }
    /* now we fill up SGE */
    this->client_send_sge.addr = (uint64_t)this->client_metadata_mr->addr;
    this->client_send_sge.length = (uint32_t)this->client_metadata_mr->length;
    this->client_send_sge.lkey = this->client_metadata_mr->lkey;
    /* now we link to the send work request */
    bzero(&this->client_send_wr, sizeof(this->client_send_wr));
    this->client_send_wr.sg_list = &this->client_send_sge;
    this->client_send_wr.num_sge = 1;
    this->client_send_wr.opcode = IBV_WR_SEND;
    this->client_send_wr.send_flags = IBV_SEND_SIGNALED;
    /* Now we post it */
    ret = ibv_post_send(this->client_qp, &this->client_send_wr,
                        &this->bad_client_send_wr);
    if (ret) {
        rdma_error("Failed to send client metadata, errno: %d \n", -errno);
        return -errno;
    }
    /* at this point we are expecting 2 work completion. One for our
     * send and one for recv that we will get from the server for
     * its buffer information */
    ret = process_work_completion_events(this->io_completion_channel, wc, 2);
    if (ret != 2) {
        rdma_error("We failed to get 2 work completions , ret = %d \n", ret);
        return ret;
    }
    debug("Server sent us its buffer location and credentials, showing \n");
    show_rdma_buffer_attr(&server_metadata_attr);
    return 0;
}

int RdmaClient::client_register_data_mr(SimpleBuffer *pBuf, uint32_t size) {
    // struct ibv_wc wc;
    int ret = -1;
    int flags = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                IBV_ACCESS_REMOTE_READ;
    /*
    this->client_read_mr =
        rdma_buffer_register(this->pd, pBuf->src, size,  // strlen(pBuf->src),
                             (ibv_access_flags)flags);
    // (ibv_access_flags)(IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
    // IBV_ACCESS_REMOTE_READ));
    if (!this->client_read_mr) {
        rdma_error("We failed to create the destination buffer, -ENOMEM\n");
        return -ENOMEM;
    }*/
    this->recvRsp.Attach(this->pd, flags);
    return 0;
}
/* This function does :
 * * Prepare memory buffers for RDMA operations
 * 1) RDMA write from src -> remote buffer
 * 2) RDMA read from remote bufer -> dst
 */
int RdmaClient::client_remote_memory_write(
    /*SimpleBuffer* pBuf, uint32_t size*/) {
    struct ibv_wc wc;
    int ret = -1;
    /* Step 1: is to copy the local buffer into the remote buffer. We will
     * reuse the previous variables. */
    /* now we fill up SGE */
    // this->client_send_sge.addr = (uint64_t)this->client_write_mr->addr;
    // this->client_send_sge.length = (uint32_t)this->client_write_mr->length;
    // this->client_send_sge.lkey = this->client_write_mr->lkey;
    this->client_send_sge.addr = (uint64_t)this->recvReq.get_mr()->addr;
    this->client_send_sge.length = (uint32_t)this->recvReq.get_mr()->length;
    this->client_send_sge.lkey = this->recvReq.get_mr()->lkey;
    /* now we link to the send work request */
    bzero(&this->client_send_wr, sizeof(this->client_send_wr));
    this->client_send_wr.sg_list = &this->client_send_sge;
    this->client_send_wr.num_sge = 1;
    this->client_send_wr.opcode = IBV_WR_RDMA_WRITE;
    this->client_send_wr.send_flags = IBV_SEND_SIGNALED;
    /* we have to tell server side info for RDMA */
    this->client_send_wr.wr.rdma.rkey = server_metadata_attr.stag.remote_stag;
    this->client_send_wr.wr.rdma.remote_addr = server_metadata_attr.address;
    /* Now we post it */
    ret = ibv_post_send(this->client_qp, &this->client_send_wr,
                        &this->bad_client_send_wr);
    if (ret) {
        rdma_error("Failed to write client src buffer, errno: %d \n", -errno);
        return -errno;
    }
    /* at this point we are expecting 1 work completion for the write */
    ret = process_work_completion_events(this->io_completion_channel, &wc, 1);
    if (ret != 1) {
        rdma_error("We failed to get 1 work completions , ret = %d \n", ret);
        return ret;
    }
    debug("Client side WRITE is complete \n");
    return 0;
}

int RdmaClient::client_remote_memory_read(
    /*SimpleBuffer* pBuf, uint32_t size*/) {
    struct ibv_wc wc;
    int ret = -1;
    /* Now we prepare a READ using same variables but for destination */
    // this->client_send_sge.addr = (uint64_t)this->client_read_mr->addr;
    // this->client_send_sge.length = (uint32_t)this->client_read_mr->length;
    // this->client_send_sge.lkey = this->client_read_mr->lkey;
    this->client_send_sge.addr = (uint64_t)this->recvRsp.get_mr()->addr;
    this->client_send_sge.length = (uint32_t)this->recvRsp.get_mr()->length;
    this->client_send_sge.lkey = this->recvRsp.get_mr()->lkey;
    /* now we link to the send work request */
    bzero(&this->client_send_wr, sizeof(this->client_send_wr));
    this->client_send_wr.sg_list = &this->client_send_sge;
    this->client_send_wr.num_sge = 1;
    this->client_send_wr.opcode = IBV_WR_RDMA_READ;
    this->client_send_wr.send_flags = IBV_SEND_SIGNALED;
    /* we have to tell server side info for RDMA */
    this->client_send_wr.wr.rdma.rkey = server_metadata_attr.stag.remote_stag;
    this->client_send_wr.wr.rdma.remote_addr = server_metadata_attr.address;
    /* Now we post it */
    ret = ibv_post_send(this->client_qp, &this->client_send_wr,
                        &this->bad_client_send_wr);
    if (ret) {
        rdma_error(
            "Failed to read client dst buffer from the master, errno: %d \n",
            -errno);
        return -errno;
    }
    /* at this point we are expecting 1 work completion for the write */
    ret = process_work_completion_events(this->io_completion_channel, &wc, 1);
    if (ret != 1) {
        rdma_error("We failed to get 1 work completions , ret = %d \n", ret);
        return ret;
    }
    debug("Client side READ is complete \n");
    return 0;
}

/* This function disconnects the RDMA connection from the server and cleans up
 * all the resources.
 */
int RdmaClient::client_disconnect_and_clean(SimpleBuffer *pBuf) {
    struct rdma_cm_event *cm_event = NULL;
    int ret = -1;
    /* active disconnect from the client side */
    ret = rdma_disconnect(this->cm_client_id);
    if (ret) {
        rdma_error("Failed to disconnect, errno: %d \n", -errno);
        // continuing anyways
    }
    ret = process_rdma_cm_event(this->cm_event_channel,
                                RDMA_CM_EVENT_DISCONNECTED, &cm_event);
    if (ret) {
        rdma_error("Failed to get RDMA_CM_EVENT_DISCONNECTED event, ret = %d\n",
                   ret);
        // continuing anyways
    }
    ret = rdma_ack_cm_event(cm_event);
    if (ret) {
        rdma_error("Failed to acknowledge cm event, errno: %d\n", -errno);
        // continuing anyways
    }
    /* Destroy QP */
    rdma_destroy_qp(this->cm_client_id);
    /* Destroy client cm id */
    ret = rdma_destroy_id(this->cm_client_id);
    if (ret) {
        rdma_error("Failed to destroy client id cleanly, %d \n", -errno);
        // we continue anyways;
    }
    /* Destroy CQ */
    ret = ibv_destroy_cq(this->client_cq);
    if (ret) {
        rdma_error("Failed to destroy completion queue cleanly, %d \n", -errno);
        // we continue anyways;
    }
    /* Destroy completion channel */
    ret = ibv_destroy_comp_channel(this->io_completion_channel);
    if (ret) {
        rdma_error("Failed to destroy completion channel cleanly, %d \n",
                   -errno);
        // we continue anyways;
    }
    /* Destroy memory buffers */
    rdma_buffer_deregister(this->server_metadata_mr);
    rdma_buffer_deregister(this->client_metadata_mr);
    // rdma_buffer_deregister(this->client_write_mr);
    this->recvReq.DeAttach();
    // rdma_buffer_deregister(this->client_read_mr);
    this->recvRsp.DeAttach();
    /* Destroy protection domain */
    ret = ibv_dealloc_pd(this->pd);
    if (ret) {
        rdma_error("Failed to destroy client protection domain cleanly, %d \n",
                   -errno);
        // we continue anyways;
    }
    rdma_destroy_event_channel(this->cm_event_channel);
    printf("Client resource clean up is complete \n");
    return 0;
}
