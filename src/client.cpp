/*
 * An example RDMA client side code.
 * Author: Animesh Trivedi
 *         atrivedi@apache.org
 */

#include "rdma_client.h"

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
                } catch (const std::invalid_argument &) {
                    usage();
                }

                rdmaClient.recvReq.Allocate(bufSize);
                memset(rdmaClient.recvReq.pbuf, 'b', bufSize);
                rdmaClient.recvRsp.Allocate(bufSize);
                memset(rdmaClient.recvRsp.pbuf, 0, bufSize);
                // bufSize = strlen(optarg);
                /* Copy the passes arguments */
                // strncpy(recvReq.src, optarg, strlen(optarg));

                // recvRsp.src = calloc(strlen(optarg), 1);
                break;
            case 'a':
                /* remember, this overwrites the port info */
                ret = get_addr(optarg, (struct sockaddr *)&server_sockaddr);
                check_ret_and_error(ret, "Invalid IP", optarg);
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
    if (rdmaClient.recvReq.pbuf == NULL) {
        printf("Please provide a string to copy \n");
        usage();
    }
    ret = rdmaClient.client_prepare_connection(&server_sockaddr);
    check_ret_and_return(ret, "Failed to setup client connection ");
    // if (ret) {
    // rdma_error("Failed to setup client connection , ret = %d \n", ret);
    // return ret;
    // }
    ret = rdmaClient.client_prepare_recv_buffer_meta();
    check_ret_and_return(ret, "Failed to setup client connection ");
    // if (ret) {
    // rdma_error("Failed to setup client connection , ret = %d \n", ret);
    // return ret;
    // }

    ret = rdmaClient.client_connect_to_server();
    check_ret_and_return(ret, "Failed to setup client connection ");
    // if (ret) {
    // rdma_error("Failed to setup client connection , ret = %d \n", ret);
    // return ret;
    // }
    // Create MR for writing and bind with req buffer
    ret = rdmaClient.client_xchange_metadata_with_server(&(rdmaClient.recvReq));
    check_ret_and_return(ret, "Failed to setup client connection ");
    // if (ret) {
    // rdma_error("Failed to setup client connection , ret = %d \n", ret);
    // return ret;
    // }

    // Create MR for reading and bind with rsp buffer
    ret = rdmaClient.client_register_data_mr(&(rdmaClient.recvRsp),
                                             rdmaClient.recvReq.length);
    check_ret_and_return(ret, "Failed to register local mr for writing");
    // if (ret) {
    // rdma_error("Failed to register local mr for writing, ret = %d \n", ret);
    // return ret;
    // }

    for (int i = 0; i < 10; i++) {
        spdlog::info("{}th send", i);
        *(char *)(rdmaClient.recvReq.pbuf) = 'a';
        *(char *)(rdmaClient.recvReq.pbuf + 1) = 'a';
        ret = rdmaClient.client_remote_memory_write();
        check_ret_and_return(ret, "Failed to finish remote memory ops");
        // if (ret) {
        // rdma_error("Failed to finish remote memory ops, ret = %d \n", ret);
        // return ret;
        // }
        rdmaClient.block_check_io_complete();
        sleep(1);
    }
    ret = rdmaClient.client_remote_memory_read();
    check_ret_and_error(ret, "Failed to finish remote memory ops");
    // if (ret) {
    // rdma_error("Failed to finish remote memory ops, ret = %d \n", ret);
    // }
    rdmaClient.block_check_io_complete();

    if (check_src_dst(rdmaClient.recvReq.pbuf, rdmaClient.recvRsp.pbuf)) {
        rdma_error("src and dst buffers do not match");
    }
    ret = rdmaClient.client_disconnect_and_clean(&rdmaClient.recvReq);
    check_ret_and_error(ret,
                        "Failed to cleanly disconnect and clean up resources");
    // if (ret) {
    // rdma_error("Failed to cleanly disconnect and clean up resources \n");
    // }
    return ret;
}

