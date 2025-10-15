/*
 * This is a RDMA server side code.
 *
 * Author: Animesh Trivedi
 *         atrivedi@apache.org
 *
 * TODO: Cleanup previously allocated resources in case of an error condition
 */

#include "rdma_common.h"

/* These are the RDMA resources needed to setup an RDMA connection */
/* Event channel, where connection management (cm) related events are relayed */
RdmaServer rdmaServer;

void usage() {
    printf("Usage:\n");
    printf("rdma_server: [-a <server_addr>] [-p <server_port>]\n");
    printf("(default port is %d)\n", DEFAULT_RDMA_PORT);
    exit(1);
}

int main(int argc, char **argv) {
    int ret, option;
    struct sockaddr_in server_sockaddr;
    bzero(&server_sockaddr, sizeof server_sockaddr);
    server_sockaddr.sin_family = AF_INET; /* standard IP NET address */
    server_sockaddr.sin_addr.s_addr = htonl(INADDR_ANY); /* passed address */
    /* Parse Command Line Arguments, not the most reliable code */
    while ((option = getopt(argc, argv, "a:p:")) != -1) {
        switch (option) {
            case 'a':
                /* Remember, this will overwrite the port info */
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
        /* If still zero, that mean no port info provided */
        server_sockaddr.sin_port =
            htons(DEFAULT_RDMA_PORT); /* use default port */
    }
    ret = rdmaServer.start_rdma_server(&server_sockaddr);
    if (ret) {
        rdma_error("RDMA server failed to start cleanly, ret = %d \n", ret);
        return ret;
    }
    ret = rdmaServer.setup_client_resources();
    if (ret) {
        rdma_error("Failed to setup client resources, ret = %d \n", ret);
        return ret;
    }
    ret = rdmaServer.accept_client_connection();
    if (ret) {
        rdma_error("Failed to handle client cleanly, ret = %d \n", ret);
        return ret;
    }
    ret = rdmaServer.send_server_metadata_to_client();
    if (ret) {
        rdma_error("Failed to send server metadata to the client, ret = %d \n",
                   ret);
        return ret;
    }
    ret = rdmaServer.disconnect_and_cleanup();
    if (ret) {
        rdma_error("Failed to clean up resources properly, ret = %d \n", ret);
        return ret;
    }
    return 0;
}
