/*
 * An example RDMA client side code.
 * Author: Animesh Trivedi
 *         atrivedi@apache.org
 */

#include "rdma_common.h"

/* These are basic RDMA resources */
/* These are RDMA connection related resources */
RdmaClient rdmaClient;
uint32_t bufSize = 0;

void usage() {
    spdlog::info("Usage:");
    spdlog::info(
        "rdma_client: [-a <server_addr>] [-p <server_port>] -s sizeofbuf"
        "(required)");
    spdlog::info("(default IP is 127.0.0.1 and port is {})", DEFAULT_RDMA_PORT);
    exit(1);
}

int main(int argc, char **argv) {
    struct sockaddr_in server_sockaddr;
    int ret, option;
    bzero(&server_sockaddr, sizeof server_sockaddr);
    server_sockaddr.sin_family = AF_INET;
    server_sockaddr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    while ((option = getopt(argc, argv, "s:a:p:")) != -1) {
        switch (option) {
            case 's':
                printf("Passed string is : %s , with count %u \n", optarg,
                       (unsigned int)strlen(optarg));

                try {
                    unsigned long val = std::stoul(optarg);  // 基于 10 进制
                    if (val > UINT32_MAX) {
                        return false;  // 超出 uint32 范围
                    }
                    bufSize = static_cast<uint32_t>(val);
                } catch(const std::invalid_argument&){
                    usage();
                } catch(const std::invalid_argument&){
                    usage();
                }

                rdmaClient.recvReq.Allocate(bufSize);
                memset(rdmaClient.recvReq.src, 'b', bufSize);
                rdmaClient.recvRsp.Allocate(bufSize);
                memset(rdmaClient.recvRsp.src, 0, bufSize);
                // bufSize = strlen(optarg);
                /* Copy the passes arguments */
                // strncpy(recvReq.src, optarg, strlen(optarg));

                // recvRsp.src = calloc(strlen(optarg), 1);
                break;
            case 'a':
                /* remember, this overwrites the port info */
                ret = get_addr(optarg, (struct sockaddr *)&server_sockaddr);
                if (ret) {
                    rdma_error("Invalid IP \n");
                    return ret;
                }
                break;
            case 'p':
                /* passed port to listen on */
                server_sockaddr.sin_port = htons(strtol(optarg, NULL, 0));
                break;
            default:
                usage();
                break;
        }
    }
    if (!server_sockaddr.sin_port) {
        /* no port provided, use the default port */
        server_sockaddr.sin_port = htons(DEFAULT_RDMA_PORT);
    }
    if (rdmaClient.recvReq.src == NULL) {
        printf("Please provide a string to copy \n");
        usage();
    }
    ret = rdmaClient.client_prepare_connection(&server_sockaddr);
    if (ret) {
        rdma_error("Failed to setup client connection , ret = %d \n", ret);
        // spdlog::error("Failed to setup client connection:{}-{}\n", error,
        // rdma_strerror(errno));
        return ret;
    }
    ret = rdmaClient.client_pre_post_recv_buffer();
    if (ret) {
        rdma_error("Failed to setup client connection , ret = %d \n", ret);
        return ret;
    }
    ret = rdmaClient.client_connect_to_server();
    if (ret) {
        rdma_error("Failed to setup client connection , ret = %d \n", ret);
        return ret;
    }
    // Create MR for writing and bind with req buffer
    ret = rdmaClient.client_xchange_metadata_with_server(&(rdmaClient.recvReq));
    if (ret) {
        rdma_error("Failed to setup client connection , ret = %d \n", ret);
        return ret;
    }

    // Create MR for reading and bind with rsp buffer
    ret = rdmaClient.client_register_data_mr(&(rdmaClient.recvRsp), rdmaClient.recvReq.length);
    if (ret) {
        rdma_error("Failed to register local mr for writing, ret = %d \n", ret);
        return ret;
    }

    for (int i = 0; i < 10; i++) {
        spdlog::info("{}th send", i);
        *(char *)(rdmaClient.recvReq.src) = 'a';
        *(char *)(rdmaClient.recvReq.src+1) = 'a';
        ret =
            rdmaClient.client_remote_memory_write(/*&recvRsp, recvReq.length*/);
        if (ret) {
            rdma_error("Failed to finish remote memory ops, ret = %d \n", ret);
            return ret;
        }
        sleep(1);
    }
    ret = rdmaClient.client_remote_memory_read(/*&recvRsp, recvReq.length*/);
    if (ret) {
        rdma_error("Failed to finish remote memory ops, ret = %d \n", ret);
    }
    if (check_src_dst(rdmaClient.recvReq.src, rdmaClient.recvRsp.src)) {
        rdma_error("src and dst buffers do not match");
    }
    ret = rdmaClient.client_disconnect_and_clean(&rdmaClient.recvReq);
    if (ret) {
        rdma_error("Failed to cleanly disconnect and clean up resources \n");
    }
    return ret;
}

