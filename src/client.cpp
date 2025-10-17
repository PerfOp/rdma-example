/*
 * An example RDMA client side code.
 * Author: Animesh Trivedi
 *         atrivedi@apache.org
 */

#include "rdma_common.h"

/* These are basic RDMA resources */
/* These are RDMA connection related resources */
RdmaClient rdmaClient;
SimpleBuffer recvReq;
SimpleBuffer recvRsp;

void usage() {
    spdlog::info("Usage:");
    spdlog::info(
        "rdma_client: [-a <server_addr>] [-p <server_port>] -s string "
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
                //recvReq.src = calloc(strlen(optarg), 1);
                recvReq.Allocate(strlen(optarg));
                if (!recvReq.src) {
                    rdma_error("Failed to allocate memory : -ENOMEM\n");
                    return -ENOMEM;
                }
                /* Copy the passes arguments */
                strncpy(recvReq.src, optarg, strlen(optarg));

                // recvRsp.src = calloc(strlen(optarg), 1);
                recvRsp.Allocate(strlen(optarg));
                if (!recvRsp.src) {
                    rdma_error(
                        "Failed to allocate destination memory, -ENOMEM\n");
                    // free(recvReq.src);
                    return -ENOMEM;
                }
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
    if (recvReq.src == NULL) {
        printf("Please provide a string to copy \n");
        usage();
    }
    ret = rdmaClient.client_prepare_connection(&server_sockaddr);
    if (ret) {
        rdma_error("Failed to setup client connection , ret = %d \n", ret);
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
    ret = rdmaClient.client_xchange_metadata_with_server(&recvReq);
    if (ret) {
        rdma_error("Failed to setup client connection , ret = %d \n", ret);
        return ret;
    }
    ret = rdmaClient.client_remote_memory_ops(&recvRsp, recvReq.length);
    if (ret) {
        rdma_error("Failed to finish remote memory ops, ret = %d \n", ret);
        return ret;
    }
    // printf("begin checking...\n");
    // spdlog::info("{} {}", recvReq.length, recvRsp.length);
    if (check_src_dst(recvReq.src, recvRsp.src)) {
        rdma_error("src and dst buffers do not match");
    }
    ret = rdmaClient.client_disconnect_and_clean(&recvReq);
    if (ret) {
        rdma_error("Failed to cleanly disconnect and clean up resources \n");
    }
    return ret;
}

