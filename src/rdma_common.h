/*
 * Header file for the common RDMA routines used in the server/client example
 * program.
 *
 * Author: Animesh Trivedi
 *          atrivedi@apache.org
 *
 */

#ifndef RDMA_COMMON_H
#define RDMA_COMMON_H

#include <arpa/inet.h>
#include <errno.h>
#include <getopt.h>
#include <infiniband/verbs.h>
#include <netdb.h>
#include <netinet/in.h>
#include <rdma/rdma_cma.h>
#include <spdlog/spdlog.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>

/* Error Macro*/
#define rdma_error(msg, args...)             \
    do {                                     \
        spdlog::error("{} {}", msg, ##args); \
    } while (0);

#define rdma_cm_event_status(event, status, args...)                    \
    do {                                                                \
        spdlog::error("cm_event:{} - status:{}", rdma_event_str(event), \
                      strerror(-status), ##args);                       \
    } while (0);
// do {fprintf(stderr, "%s : %d : ERROR : %s\n", __FILE__, __LINE__, msg,
// ##__VA_ARGS__);}while(0);
//  fprintf(stderr, "%s : %d : ERROR : "msg, __FILE__, __LINE__, ## args);\


#ifdef ACN_RDMA_DEBUG
/* Debug Macro */
#define debug(msg, args...)                  \
    do {                                     \
        spdlog::debug("{} {} " msg, ##args); \
    } while (0);

#else

#define debug(msg, args...)

#endif /* ACN_RDMA_DEBUG */

/* Capacity of the completion queue (CQ) */
#define CQ_CAPACITY (16)
/* MAX SGE capacity */
#define MAX_SGE (2)
/* MAX work requests */
#define MAX_WR (8)
/* Default port where the RDMA server is listening */
#define DEFAULT_RDMA_PORT (20886)

/*
 * 1. We use attribute so that compiler does not step in and try to pad the
 * structure.
 * 2. We use this structure to exchange information between the server
 * and the client.
 *
 * For details see: http://gcc.gnu.org/onlinedocs/gcc/Type-Attributes.html
 */
struct __attribute((packed)) RdmaBufferAttr {
    uint64_t address;
    uint32_t length;
    union stag {
        /* if we send, we call it local stags */
        uint32_t local_stag;
        /* if we receive, we call it remote stag */
        uint32_t remote_stag;
    } stag;
};
/* resolves a given destination name to sin_addr */
int get_addr(char *dst, struct sockaddr *addr);

/* prints RDMA buffer info structure */
void show_rdma_buffer_attr(struct RdmaBufferAttr *attr);

/*
 * Processes an RDMA connection management (CM) event.
 * @echannel: CM event channel where the event is expected.
 * @expected_event: Expected event type
 * @cm_event: where the event will be stored
 */
int process_rdma_cm_event(struct rdma_event_channel *echannel,
                          enum rdma_cm_event_type expected_event,
                          struct rdma_cm_event **cm_event);

/* Allocates an RDMA buffer of size 'length' with permission.
 * Register the memory under PD and returns a memory region (MR)
 * identifier or NULL on error.
 * alloc : allocating memory and register it with PD.
 * @pd: Protection domain where the buffer should be allocated
 * @length: Length of the buffer
 * @permission: OR of IBV_ACCESS_* permissions as defined for the enum
 * ibv_access_flags
 */
struct ibv_mr *rdma_buffer_alloc(struct ibv_pd *pd, uint32_t length,
                                 enum ibv_access_flags permission);

/* Frees a previously allocated RDMA buffer. The buffer must be allocated by
 * calling rdma_buffer_alloc();
 * @mr: RDMA memory region to free
 */
void rdma_buffer_free(struct ibv_mr *mr);

/* This function registers a previously allocated memory. Returns a memory
 * region (MR) identifier or NULL on error.
 * @pd: protection domain where to register memory
 * @addr: Buffer address
 * @length: Length of the buffer
 * @permission: OR of IBV_ACCESS_* permissions as defined for the enum
 * ibv_access_flags
 */
struct ibv_mr *rdma_buffer_register(struct ibv_pd *pd, void *addr,
                                    uint32_t length,
                                    enum ibv_access_flags permission);
/* Deregisters a previously register memory
 * @mr: Memory region to deregister
 */
void rdma_buffer_deregister(struct ibv_mr *mr);

/* Processes a work completion (WC) notification.
 * @comp_channel: Completion channel where the notifications are expected to
 * arrive
 * @wc: Array where to hold the work completion elements
 * @max_wc: Maximum number of expected work completion (WC) elements. wc must be
 *          atleast this size.
 */
int process_work_completion_events(struct ibv_comp_channel *comp_channel,
                                   struct ibv_wc *wc, int max_wc);

/* prints some details from the cm id */
void show_rdma_cmid(struct rdma_cm_id *id);

extern std::atomic<bool> exit_indicator;
void terminate(int signal);
// extern char *src, *dst;

/* This is our testing function */
int check_src_dst(uint8_t *src, uint8_t *dst);

class SimpleBuffer {
private:
    struct ibv_mr *m_mr;

public:
    struct ibv_recv_wr client_recv_wr, *bad_client_recv_wr;
    struct ibv_send_wr client_send_wr, *bad_client_send_wr;
    struct ibv_sge send_sge, client_recv_sge;
    uint8_t *pbuf;
    uint32_t length;

    struct ibv_pd *m_pd;
    enum ibv_access_flags m_flags;
    // char *dst;

public:
    SimpleBuffer()
        : pbuf(nullptr),
          length(0) /*, bad_send_wr(nullptr), bad_recv_wr(nullptr)*/ {}
    virtual ~SimpleBuffer() {
        if (pbuf) {
            // free(src);
            delete[] pbuf;
            pbuf = nullptr;
        }
    }

    inline const struct ibv_mr *get_mr() { return m_mr; }
    inline const uint8_t *get_buf() { return pbuf; }

    uint32_t Allocate(uint32_t size) {
        // src = calloc(size, 1);
        pbuf = new uint8_t[size];
        if (pbuf == nullptr) {
            rdma_error("Failed to allocate memory : -ENOMEM\n");
            return 0;
        }
        length = size;
        return size;
    }

    void DeAllocate() {
        if (pbuf != nullptr) {
            delete[] pbuf;
            pbuf = nullptr;
        }
    }

    int remote_write(struct ibv_qp *client_qp,
                     const struct RdmaBufferAttr &target_srv_attr) {
        return remote_ops(client_qp, target_srv_attr, IBV_WR_RDMA_WRITE);
    }
    int remote_read(struct ibv_qp *client_qp,
                    const struct RdmaBufferAttr &target_srv_attr) {
        return remote_ops(client_qp, target_srv_attr, IBV_WR_RDMA_READ);
    }

    int remote_msg(struct ibv_qp *client_qp, enum ibv_wr_opcode opcode) {
        int ret = -1;
        send_sge.addr = (uint64_t)m_mr->addr;
        send_sge.length = (uint32_t)m_mr->length;
        send_sge.lkey = m_mr->lkey;
        /* now we link to the send work request */
        bzero(&this->client_send_wr, sizeof(this->client_send_wr));
        this->client_send_wr.sg_list = &this->send_sge;
        this->client_send_wr.num_sge = 1;
        this->client_send_wr.opcode = opcode;
        this->client_send_wr.send_flags = IBV_SEND_SIGNALED;
        /* Now we post it */
        ret = ibv_post_send(client_qp, &client_send_wr, &bad_client_send_wr);
        if (ret) {
            rdma_error("Failed to send client metadata, errno: %d \n", -errno);
            return -errno;
        }
        return 0;
    }

private:
    int remote_ops(struct ibv_qp *client_qp,
                   const struct RdmaBufferAttr &target_srv_attr,
                   enum ibv_wr_opcode opcode) {
        struct ibv_wc wc;
        int ret = -1;
        /* Now we prepare a READ using same variables but for destination */
        send_sge.addr = (uint64_t)m_mr->addr;
        send_sge.length = (uint32_t)m_mr->length;
        send_sge.lkey = m_mr->lkey;
        /* now we link to the send work request */
        bzero(&this->client_send_wr, sizeof(this->client_send_wr));
        this->client_send_wr.sg_list = &this->send_sge;
        this->client_send_wr.num_sge = 1;
        this->client_send_wr.opcode = opcode;
        this->client_send_wr.send_flags = IBV_SEND_SIGNALED;
        /* we have to tell server side info for RDMA */
        this->client_send_wr.wr.rdma.rkey = target_srv_attr.stag.remote_stag;
        this->client_send_wr.wr.rdma.remote_addr = target_srv_attr.address;
        /* Now we post it */
        ret = ibv_post_send(client_qp, &this->client_send_wr,
                            &this->bad_client_send_wr);
        if (ret) {
            rdma_error(
                "Failed to read client dst buffer from the master, errno: %d "
                "\n",
                -errno);
            return -errno;
        }
        return 0;
    }

public:
    int Attach(struct ibv_pd *pd, enum ibv_access_flags permission) {
        if (!pd) {
            rdma_error("Protection domain is NULL \n");
            return -1;
        }
        m_pd = pd;
        if (!pbuf) {
            rdma_error("attaching an invalid buffer");
            return -1;
        }
        m_mr = rdma_buffer_register(m_pd, pbuf, length, permission);
        if (!m_mr) {
            rdma_error("Failed to create mr on buffer, errno: %d \n", -errno);
            return -1;
        }
        m_flags = permission;
        debug("Buffer attached: %p , len: %u \n", pbuf, length);

        return 0;
    }

    int DeAttach() {
        if (!m_mr) {
            rdma_error("Passed memory region is NULL, ignoring\n");
            return -1;
        }
        rdma_buffer_deregister(m_mr);
        return 0;
    }
    uint32_t SyncData(void *pdata, uint32_t size) {
        if (size == 0 || pdata == nullptr || pbuf == nullptr || length < size) {
            return 0;
        }

        memcpy(pbuf, pdata, size);
        return size;
    }
};

class ClientCtx {
public:
    struct rdma_cm_id *cm_client_id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_qp *client_qp;
    struct ibv_comp_channel *io_completion_channel;
    struct ibv_qp_init_attr qp_init_attr;
    struct RdmaBufferAttr client_metadata_attr, server_metadata_attr;

public:
    ClientCtx()
        : cm_client_id(NULL),
          pd(NULL),
          cq(NULL),
          client_qp(NULL),
          io_completion_channel(NULL) {}

    int SetupCtx(struct rdma_cm_id *client_id);

    int CleanupCtx();
};

class RdmaServer {
public:
    struct rdma_cm_id *cm_client_id;

private:
    struct rdma_event_channel *cm_event_channel;
    struct rdma_cm_id *cm_server_id;

    struct ibv_mr *client_metadata_mr;
    struct ibv_mr *server_metadata_mr;
    struct ibv_recv_wr *bad_client_recv_wr;
    struct ibv_send_wr *bad_server_send_wr;

    // Variables
    struct ibv_recv_wr client_recv_wr;
    struct ibv_send_wr server_send_wr;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_sge client_recv_sge, server_send_sge;

    // struct RdmaBufferAttr client_metadata_attr, server_metadata_attr;

    SimpleBuffer serverBuffer;

public:
    ClientCtx m_clientCtx;

public:
    RdmaServer()
        : cm_event_channel(NULL),
          cm_server_id(NULL),
          cm_client_id(NULL),
          client_metadata_mr(NULL),
          server_metadata_mr(NULL),
          bad_client_recv_wr(NULL),
          bad_server_send_wr(NULL) {}

    int start_rdma_server(struct sockaddr_in *server_addr);
    // int setup_client_resources();
    int accept_client_connection();
    int send_server_metadata_to_client();
    int disconnect_and_cleanup();
    int server_cleanup();

    int block_handle_connect_event();

private:
    int prepare_to_recv_client_meta();
};

class RdmaClient {
public:
    // IO channel
    struct ibv_comp_channel *io_completion_channel;
    // RDMA_CM channel
    struct rdma_event_channel *cm_event_channel;

private:
    struct rdma_cm_id *cm_client_id;
    struct ibv_pd *pd;
    struct ibv_cq *client_cq;
    struct ibv_qp *client_qp;

    struct ibv_mr /**client_metadata_mr,*/ *server_metadata_mr;
    // Used for RDMA-write
    // struct ibv_mr *client_write_mr;
    // Used for RDMA-read
    // struct ibv_mr *client_read_mr;

    struct ibv_send_wr client_send_wr, *bad_client_send_wr;
    struct ibv_recv_wr server_recv_wr, *bad_server_recv_wr;

    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_sge client_send_sge, server_recv_sge;

    struct RdmaBufferAttr /*client_metadata_attr,*/ server_metadata_attr;
    // Generated by local rdma API, which is necessary for Server-Side write to client.
    struct RdmaBufferAttr *client_side_metadata_attr;

public:
    // Used for RDMA-write
    SimpleBuffer clientMeta;
    SimpleBuffer serverMeta;

    // Used for RDMA-write
    SimpleBuffer recvReq;
    // Used for RDMA-read
    SimpleBuffer recvRsp;

public:
    RdmaClient()
        : cm_event_channel(nullptr),
          cm_client_id(nullptr),
          pd(nullptr),
          io_completion_channel(nullptr),
          client_cq(nullptr),
          client_qp(nullptr),
          // client_metadata_mr(nullptr),
          // client_write_mr(nullptr),
          // client_read_mr(nullptr),
          server_metadata_mr(nullptr),
          bad_client_send_wr(nullptr),
          bad_server_recv_wr(nullptr) {}

    int client_prepare_connection(struct sockaddr_in *s_addr);
    int client_pre_post_recv_buffer();
    int client_connect_to_server();
    int client_xchange_metadata_with_server(SimpleBuffer *pBuf);

    int client_prepare_qp(void);
    // Start:Sending apis for sending data
    // Function: register data mr
    int client_register_data_mr(SimpleBuffer *pBuf, uint32_t size);
    // Function: regular write to remote over RDMA, Only trigger remote write
    // without the memcpy
    int client_remote_memory_write(/*SimpleBuffer *pBuf, uint32_t size*/);
    int client_remote_memory_read(/*SimpleBuffer *pBuf, uint32_t size*/);
    // Function: regular write to remote over RDMA
    // int client_remote_memory_ops_imm(SimpleBuffer *pBuf, uint32_t size);
    // End:Sending apis for sending data

    int block_check_io_complete();

    int client_disconnect_and_clean(SimpleBuffer *pBuf);
};

// extern char *src , *dst;
#endif /* RDMA_COMMON_H */
