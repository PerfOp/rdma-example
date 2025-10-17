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
int check_src_dst(char *src, char *dst);

class MemoryRegion {
private:
    void *m_pbuf;
    struct ibv_mr *m_mr;
    struct ibv_pd *m_pd;
    uint32_t m_size;
    enum ibv_access_flags m_permission;

public:
    MemoryRegion() : m_size(0), m_pbuf(NULL), m_mr(NULL), m_pd(NULL) {}
    virtual ~MemoryRegion() {}

    int Attach(const struct ibv_pd *pd, const void *buf, const uint32_t size,
               enum ibv_access_flags permission) {
        if (!pd) {
            rdma_error("Protection domain is NULL \n");
            return -1;
        }
        m_pd = pd;
        if (!buf) {
            rdma_error("attaching an invalid buffer");
            return -1;
        }
        m_mr = rdma_buffer_register(m_pd, buf, m_size, permission);
        if (!m_mr) {
            rdma_error("Failed to create mr on buffer, errno: %d \n", -errno);
            free(buf);
            return -1;
        }
        m_size = size;
        m_pbuf = buf;
        m_permission = permission;
        debug("Buffer attached: %p , len: %u \n", buf, size);
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
};

class MemoryRegionAllocator {
private:
    void *m_pbuf;
    struct ibv_mr *m_mr;
    struct ibv_pd *m_pd;
    uint32_t m_size;
    enum ibv_access_flags m_permission;

public:
    MemoryRegionAllocator() : m_size(0), m_pbuf(NULL), m_mr(NULL), m_pd(NULL) {}
    virtual ~MemoryRegionAllocator() {}
    inline const struct ibv_mr *get_mr() { return m_mr; }

    int Allocate(const struct ibv_pd *pd, const uint32_t size,
                 enum ibv_access_flags permission) {
        // Create and init a memory region and register it with PD.
        if (!pd) {
            rdma_error("Protection domain is NULL \n");
            return -1;
        }
        m_pd = pd;
        void *buf = calloc(1, size);
        if (!buf) {
            rdma_error("failed to allocate buffer, -ENOMEM\n");
            return -1;
        }
        debug("Buffer allocated: %p , len: %u \n", buf, size);
        // m_mr = ibv_reg_mr(m_pd, buf, m_size, m_permission);
        m_mr = rdma_buffer_register(m_pd, buf, m_size, permission);
        if (!m_mr) {
            rdma_error("Failed to create mr on buffer, errno: %d \n", -errno);
            free(buf);
            return -1;
        }
        m_size = size;
        m_pbuf = buf;
        m_permission = permission;
        return 0;
    }

    int DeAllocate() {
        if (!m_mr) {
            rdma_error("Passed memory region is NULL, ignoring\n");
            return -1;
        }
        void *to_free = m_mr->addr;
        rdma_buffer_deregister(m_mr);
        debug("Buffer %p free'ed\n", to_free);
        free(to_free);
        return 0;
    }
};

class RdmaServer {
private:
    struct rdma_event_channel *cm_event_channel;
    struct rdma_cm_id *cm_server_id;
    struct rdma_cm_id *cm_client_id;
    struct ibv_pd *pd;
    struct ibv_cq *cq;
    struct ibv_comp_channel *io_completion_channel;
    struct ibv_qp *client_qp;
    struct ibv_mr *client_metadata_mr;
    struct ibv_mr *server_buffer_mr;
    struct ibv_mr *server_metadata_mr;
    struct ibv_recv_wr *bad_client_recv_wr;
    struct ibv_send_wr *bad_server_send_wr;

    // Variables
    struct ibv_recv_wr client_recv_wr;
    struct ibv_send_wr server_send_wr;
    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_sge client_recv_sge, server_send_sge;

    struct RdmaBufferAttr client_metadata_attr, server_metadata_attr;

public:
    RdmaServer()
        : cm_event_channel(NULL),
          cm_server_id(NULL),
          cm_client_id(NULL),
          pd(NULL),
          cq(NULL),
          io_completion_channel(NULL),
          client_qp(NULL),
          client_metadata_mr(NULL),
          server_buffer_mr(NULL),
          server_metadata_mr(NULL),
          bad_client_recv_wr(NULL),
          bad_server_send_wr(NULL) {}

    int start_rdma_server(struct sockaddr_in *server_addr);
    int setup_client_resources();
    int accept_client_connection();
    int send_server_metadata_to_client();
    int disconnect_and_cleanup();
    int server_cleanup();

    int block_handle_connect_event();
};

class SimpleBuffer {
public:
    char *src;
    uint32_t length;
    // char *dst;

public:
    SimpleBuffer() : src(NULL), length(0)/*dst(NULL)*/ {}
    ~SimpleBuffer() {
        if (src) {
            free(src);
            src = NULL;
        }
        // if (dst) {
        // free(dst);
        // dst = NULL;
        // }
    }

    uint32_t Allocate(uint32_t size) {
        src = calloc(size, 1);
        if (src == nullptr) {
            rdma_error("Failed to allocate memory : -ENOMEM\n");
            return 0;
        }
        length=size;
        return size;
    }
};

class RdmaClient {
private:
    struct rdma_event_channel *cm_event_channel;
    struct rdma_cm_id *cm_client_id;
    struct ibv_pd *pd;
    struct ibv_comp_channel *io_completion_channel;
    struct ibv_cq *client_cq;
    struct ibv_qp *client_qp;

    struct ibv_mr *client_metadata_mr, *client_src_mr, *client_dst_mr,
        *server_metadata_mr;
    struct ibv_send_wr client_send_wr, *bad_client_send_wr;
    struct ibv_recv_wr server_recv_wr, *bad_server_recv_wr;

    struct ibv_qp_init_attr qp_init_attr;
    struct ibv_sge client_send_sge, server_recv_sge;

    struct RdmaBufferAttr client_metadata_attr, server_metadata_attr;

public:
    RdmaClient()
        : cm_event_channel(NULL),
          cm_client_id(NULL),
          pd(NULL),
          io_completion_channel(NULL),
          client_cq(NULL),
          client_qp(NULL),
          client_metadata_mr(NULL),
          client_src_mr(NULL),
          client_dst_mr(NULL),
          server_metadata_mr(NULL),
          bad_client_send_wr(NULL),
          bad_server_recv_wr(NULL) {}

    int client_prepare_connection(struct sockaddr_in *s_addr);
    int client_pre_post_recv_buffer();
    int client_connect_to_server();
    int client_xchange_metadata_with_server(SimpleBuffer *pBuf);
    int client_remote_memory_ops(SimpleBuffer *pBuf, uint32_t size);
    int client_disconnect_and_clean(SimpleBuffer *pBuf);
};

// extern char *src , *dst;
#endif /* RDMA_COMMON_H */
