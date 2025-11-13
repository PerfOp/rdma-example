/*
 * This is a RDMA server side code.
 *
 * Author: Animesh Trivedi
 *         atrivedi@apache.org
 *
 * TODO: Cleanup previously allocated resources in case of an error condition
 */
#include <csignal>
#include "rdma_server.h"

/* These are the RDMA resources needed to setup an RDMA connection */
/* Event channel, where connection management (cm) related events are relayed */
RdmaServer rdmaServer;

void usage() {
    spdlog::info("Usage:");
    spdlog::info("rdma_server: [-a <server_addr>] [-p <server_port>]");
    spdlog::info("(default port is {})", DEFAULT_RDMA_PORT);
    exit(1);
}

int main_loop() {
    int ret = -1;
    ret = rdmaServer.wait_for_connect_event();
    if (ret) {
        rdma_error("Failed to setup client resources, ret = %d \n", ret);
        return ret;
    }

    ret = rdmaServer.m_clientCtx.SetupCtx(rdmaServer.cm_client_id);
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
        rdma_error(
            "Failed to clean up resources for client properly, ret = %d \n",
            ret);
        return ret;
    }
    return ret;
}

int main(int argc, char **argv) {

    struct sigaction action;
    memset(&action, 0, sizeof(struct sigaction));
    action.sa_handler = terminate;
    sigaction(SIGTERM, &action, nullptr);
    sigaction(SIGINT, &action, nullptr);

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
    // Create necessary resource for a standby server.
    ret = rdmaServer.start_rdma_server(&server_sockaddr);
    if (ret) {
        rdma_error("RDMA server failed to start cleanly, ret = %d \n", ret);
        return ret;
    }

    // Waiting for the connection event:
    // * Get client-id from client connection.
    // * Ack it.
    while (!exit_indicator && ret == 0) {
        ret = main_loop();
    }

    spdlog::info("Cleaning...\n");
    ret = rdmaServer.server_cleanup();
    if (ret) {
        rdma_error(
            "Failed to clean up resources for server properly, ret = %d \n",
            ret);
        return ret;
    }
    return 0;
}
