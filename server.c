#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/time.h>
#include <errno.h>
#include <strings.h>
#include "protocol.h"
#include "window.h"
#include "networks.h"
#include "safeUtil.h"
#include "pollLib.h"
#include "cpe464.h"
#include "checksum.h"
#include "gethostbyname.h"
#include "circular_buffer.h"
#include "debug.h"

/* Structure for passing parameters to the client fork
   We need this to pass all the connection info to the child process
   when we fork. I forking hate forks */
typedef struct {
    struct sockaddr_in6 client_addr;  // Client's address info - need this to send packets back
    int client_addr_len;              // Length of the address structure
    char init_packet[INIT_BUF_SIZE];  // The initial packet from client with filename request
    int init_len;                     // Length of that initial packet
    double error_rate;                // Error rate for packet corruption simulation
} client_fork_params_t;

/* Function prototypes */
void handle_client(int sockfd, struct sockaddr_in6 client_addr, int client_addr_len,
                   char *init_packet, int init_len, double error_rate);
void send_data_packets(int sockfd, struct sockaddr_in6 client_addr, int client_addr_len,
                       FILE *fp, uint32_t window_size, uint32_t buffer_size, double error_rate);

/* Helper function prototypes */
static void send_filename_response(int sockfd, struct sockaddr_in6 client_addr, int client_addr_len,
                                   const char *msg, int break_after, int expect_ack);
static void process_ack_packets(int sockfd, window_t *win, struct sockaddr_in6 client_addr, int client_addr_len);
static void handle_timeout(int sockfd, window_t *win, struct sockaddr_in6 client_addr, int client_addr_len,
                           circular_buffer_t *cb, uint32_t seq_num, uint32_t window_size,
                           uint32_t buffer_size, int *active, int eof_reached, int *timeout_counter);
static void send_eof_packet(int sockfd, struct sockaddr_in6 client_addr, int client_addr_len,
                            window_t *win, uint32_t seq_num);

/* SIGCHLD handler to prevent zombie processes */
void sigchld_handler(int s) {
    while (waitpid(-1, NULL, WNOHANG) > 0);
}

int main(int argc, char *argv[]) {
    double error_rate;
    int port;
    
    /* Initialize debug mode */
    debug_init(argc, argv);
    
    /* Check for required arguments, ignoring debug flag */
    int min_args = 1; // error rate is required
    int max_args = 2; // port is optional
    int provided_args = argc - 1; // exclude program name
    
    if (argc > 1 && strcmp(argv[argc-1], "-d") == 0) {
        provided_args--;
    }
    
    if (provided_args < min_args || provided_args > max_args) {
        fprintf(stderr, "Usage: %s error-rate [optional port number] [-d]\n", argv[0]);
        exit(1);
    }
    
    error_rate = atof(argv[1]);
    if (provided_args == max_args) {
        // If we have all arguments, the port is either the 2nd or 3rd argument
        // depending on whether a debug flag was provided
        int port_arg = 2;
        if (argc > 3 && strcmp(argv[3], "-d") == 0) {
            port_arg = 2;
        } else if (argc > 3) {
            port_arg = 3;
        }
        port = atoi(argv[port_arg]);
    } else {
        port = 0;  // let system choose port
    }

    /* Initialize error injection for main process */
    sendtoErr_init(error_rate, DROP_ON, FLIP_ON, DEBUG_OFF, RSEED_ON);
    
    /* Set up signal handler to reap child processes */
    struct sigaction sa;
    sa.sa_handler = sigchld_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    if (sigaction(SIGCHLD, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    int sockfd = udpServerSetup(port);
    if (sockfd < 0) {
        fprintf(stderr, "Error setting up UDP server socket\n");
        exit(1);
    }
    
    setupPollSet();
    addToPollSet(sockfd);

    /* Print the port number in use */
    struct sockaddr_in6 addr;
    int addrlen = sizeof(addr);
    getsockname(sockfd, (struct sockaddr *)&addr, (socklen_t *)&addrlen);
    // printf("Server is using port %d\n", ntohs(addr.sin6_port));

    /* Main loop: wait for an initial filename packet */
    while (1) {
        char buffer[INIT_BUF_SIZE];
        struct sockaddr_in6 client_addr;
        int client_addr_len = sizeof(client_addr);
        
        int recv_len = safeRecvfrom(sockfd, buffer, INIT_BUF_SIZE, 0,
                                    (struct sockaddr *)&client_addr, &client_addr_len);
        
        if (recv_len >= HEADER_SIZE) {
            pdu_header_t *header = (pdu_header_t *)buffer;
            uint16_t recv_chk = header->checksum;
            header->checksum = 0;
            uint16_t calc_chk = in_cksum((unsigned short *)buffer, recv_len);
            
            if (recv_chk != calc_chk) {
                DEBUG_PRINT("Dropping corrupted initial packet.\n");
                continue;
            }
            
            if (header->flag == FLAG_FILENAME) {
                DEBUG_PRINT("Received filename request packet from client.\n");

                /* Fork a child process to handle this client */
                pid_t pid = fork();
                if (pid < 0) {
                    perror("fork");
                    continue;
                } 
                else if (pid == 0) {
                    /* Child process */
                    close(sockfd);  // Child doesn't need the listening socket
                    
                    /* Create a new UDP socket for this client */
                    int client_sockfd = udpServerSetup(0);  // system assigns an ephemeral port
                    if (client_sockfd < 0) {
                        fprintf(stderr, "Error setting up UDP socket for client process\n");
                        exit(1);
                    }
                    
                    // Get and display port number for child process
                    struct sockaddr_in6 child_addr;
                    int child_addrlen = sizeof(child_addr);
                    getsockname(client_sockfd, (struct sockaddr *)&child_addr, (socklen_t *)&child_addrlen);
                    
                    DEBUG_PRINT("==== CHILD PROCESS CREATED (PID: %d) ====\n", getpid());
                    DEBUG_PRINT("Child server using port %d to serve client at %s:%d\n", 
                                ntohs(child_addr.sin6_port),
                                ipAddressToString(&client_addr), 
                                ntohs(client_addr.sin6_port));
                    
                    /* Reinitialize error injection for child process */
                    sendtoErr_init(error_rate, DROP_ON, FLIP_ON, DEBUG_OFF, RSEED_ON);
                    
                    /* Process the client's request */
                    handle_client(client_sockfd, client_addr, client_addr_len, buffer, recv_len, error_rate);
                    
                    /* Clean up and exit child process */
                    DEBUG_PRINT("==== CHILD PROCESS TERMINATING (PID: %d) ====\n", getpid());
                    close(client_sockfd);
                    exit(0);
                }
                DEBUG_PRINT("Parent process created child with PID: %d to handle client\n", pid);
                /* Parent continues to listen for more connections */
            }
        }
    }
    
    close(sockfd);
    return 0;
}

/* This function handles one client connection in the child process
   It's called after we fork() to serve a specific client
   The parent process keeps listening for more clients */
void handle_client(int sockfd, struct sockaddr_in6 client_addr, int client_addr_len,
                   char *init_packet, int init_len, double error_rate) {
    // First let's make sure the packet is big enough to contain a valid request
    // malformed/cooked packets are bad
    if (init_len < HEADER_SIZE + sizeof(init_payload_t)) {
        DEBUG_PRINT("Malformed filename request packet.\n");
        return;
    }
    
    // Extract the filename and parameters from the packet in network order
    init_payload_t init_payload;
    memcpy(&init_payload, init_packet + HEADER_SIZE, sizeof(init_payload_t));
    uint32_t window_size = ntohl(init_payload.window_size);  // Convert from network to host byte order
    uint32_t buffer_size = ntohl(init_payload.buffer_size);  // Same for buffer size

    // Log what the client is asking for
    DEBUG_PRINT("Client requested file: %s\n", init_payload.filename);
    DEBUG_PRINT("Client window size: %u, buffer size: %u bytes\n", window_size, buffer_size);

    /* Now we try to open the file  */
    FILE *fp = fopen(init_payload.filename, "rb");  // "rb" for binary mode
    if (!fp) {
        // Oops, couldn't find the file! Tell the client.
        DEBUG_PRINT("File %s not found. Sending error response.\n", init_payload.filename);
        send_filename_response(sockfd, client_addr, client_addr_len, "File not found", 3, 0);
        return;
    } else {
        // Found the file! Send an OK and prepare to transfer the file
        DEBUG_PRINT("Opened file %s successfully. Sending OK response.\n", init_payload.filename);
        send_filename_response(sockfd, client_addr, client_addr_len, "OK", 3, 1);
    }
    
    /* Now for the real work - sending the actual file data */
    send_data_packets(sockfd, client_addr, client_addr_len, fp, window_size, buffer_size, error_rate);
    fclose(fp);
}

/* Helper function to send filename response with retries.
   'expect_ack' should be non-zero if waiting for an acknowledgment (as in the OK case),
   and 'break_after' controls the maximum retry count for early exit. */
static void send_filename_response(int sockfd, struct sockaddr_in6 client_addr, int client_addr_len,
                                   const char *msg, int break_after, int expect_ack) {
    int msg_len = strlen(msg) + 1;  // Include null terminator
    char resp_buf[HEADER_SIZE + 50];  // Buffer for response packet
    pdu_header_t resp_header;
    
    // Prepare the header in network byte order
    resp_header.seq_num = htonl(0);  // Sequence number 0 for filename response
    resp_header.flag    = FLAG_FILENAME_RESP;  // Set flag for filename response
    resp_header.checksum = 0;  // Initialize checksum to 0 for calculation
    
    // Copy header and message to buffer
    memcpy(resp_buf, &resp_header, HEADER_SIZE);
    memcpy(resp_buf + HEADER_SIZE, msg, msg_len);
    
    // Calculate checksum and update header in buffer
    resp_header.checksum = in_cksum((unsigned short *)resp_buf, HEADER_SIZE + msg_len);
    memcpy(resp_buf, &resp_header, HEADER_SIZE);

    DEBUG_PRINT("Preparing filename response: \"%s\"\n", msg);
    // THESE SAFESENDTO ARE SENDTOERR I BELIEVE FROM MY UNDERSTANDING OF CPE464.H
    // I hope I'm right or rip grade
    int retries = 0;
    int ack_received = 0;
    
    // Retry loop for sending filename response
    while (retries < MAX_RETRANSMIT && (!expect_ack || !ack_received)) {
        DEBUG_PRINT("Sending filename response packet (attempt %d/%d)\n", retries+1, MAX_RETRANSMIT);
        safeSendto(sockfd, resp_buf, HEADER_SIZE + msg_len, 0,
                 (struct sockaddr *)&client_addr, client_addr_len);
        
        if (expect_ack) {
            DEBUG_PRINT("Waiting for client acknowledgment...\n");
        }
        
        // Set up polling to wait for response
        setupPollSet();
        addToPollSet(sockfd);
        
        // Wait for 1 second for a response
        if (pollCall(1000) > 0) {
            char retry_buf[INIT_BUF_SIZE];
            struct sockaddr_in6 retry_addr;
            int retry_addr_len = sizeof(retry_addr);
            
            // Receive incoming packet
            int retry_len = safeRecvfrom(sockfd, retry_buf, INIT_BUF_SIZE, 0,
                                         (struct sockaddr *)&retry_addr, &retry_addr_len);
            
            // Process the received packet if it's valid
            if (retry_len >= HEADER_SIZE) {
                pdu_header_t *retry_header = (pdu_header_t *)retry_buf;
                
                // Check if it's a filename request (client acknowledgment)
                if (retry_header->flag == FLAG_FILENAME) {
                    if (expect_ack) {
                        DEBUG_PRINT("Received client acknowledgment for filename response\n");
                        ack_received = 1;
                        break;
                    } else {
                        DEBUG_PRINT("Received another filename request, continuing retry\n");
                        continue;
                    }
                }
            }
        }
        
        // Increment retry counter and check if we should break early
        retries++;
        if (retries >= break_after)
            break;
    }
    
    if (expect_ack && !ack_received) {
        DEBUG_PRINT("Failed to receive acknowledgment for filename response after %d attempts\n", retries);
    }
}

/* Helper function to process pending acknowledgment packets */
static void process_ack_packets(int sockfd, window_t *win, struct sockaddr_in6 client_addr, int client_addr_len) {
    // Check if there are any packets to process (non-blocking poll)
    int poll_result = pollCall(0);
    
    if (poll_result > 0) {
        DEBUG_PRINT("Processing incoming acknowledgment packets\n");
    }
    
    // Process all available packets
    while (poll_result > 0) {
        char ack_buf[HEADER_SIZE + 4];  // Buffer for ACK packet (header + 4 bytes for sequence number)
        struct sockaddr_in6 recv_addr;
        int recv_addr_len = sizeof(recv_addr);
        
        // Receive the packet
        int recv_len = safeRecvfrom(sockfd, ack_buf, sizeof(ack_buf), 0,
                                    (struct sockaddr *)&recv_addr, &recv_addr_len);
        
        // Process if packet is valid
        if (recv_len >= HEADER_SIZE + 4) {
            pdu_header_t *ack_header = (pdu_header_t *)ack_buf;
            
            // Verify checksum
            uint16_t recv_chk = ack_header->checksum;
            ack_header->checksum = 0;
            uint16_t calc_chk = in_cksum((unsigned short *)ack_buf, recv_len);
            
            if (recv_chk != calc_chk) {
                DEBUG_PRINT("Received corrupted ack packet. Ignoring.\n");
            } 
            // Process RR (Ready to Receive) acknowledgment
            else if (ack_header->flag == FLAG_RR) {
                // Extract sequence number from packet
                uint32_t ack_seq;
                memcpy(&ack_seq, ack_buf + HEADER_SIZE, 4);
                ack_seq = ntohl(ack_seq);  // Convert from network to host byte order
                
                DEBUG_PRINT("ACK: Received RR for seq=%u (window base=%u, window end=%u)\n", 
                          ack_seq, win->base, win->base + win->window_size - 1);
                
                // Track repeated RRs to detect potential packet loss
                static uint32_t last_rr_seq = 0;
                static int repeat_rr_count = 0;
                
                // If we get the same RR multiple times, it might indicate the client is missing a packet
                if (last_rr_seq == ack_seq && ack_seq == win->base - 1) {
                    repeat_rr_count++;
                    DEBUG_PRINT("Detected repeated RR for seq: %u (%d times)\n", ack_seq, repeat_rr_count);
                    
                    // After 3 repeated RRs, assume packet loss and retransmit
                    if (repeat_rr_count >= 3) {
                        DEBUG_PRINT("Client appears to be missing packet at window base (%u)\n", win->base);
                        
                        // Find the packet at the window base
                        int base_index = win->base % win->window_size;
                        packet_t *base_pkt = NULL;
                        
                        // Check if packet is at the expected index
                        if (win->packets[base_index].seq_num == win->base &&
                            win->packets[base_index].data != NULL) {
                            base_pkt = &win->packets[base_index];
                        } 
                        // If not, search through the window
                        else {
                            for (int i = 0; i < win->window_size; i++) {
                                int check_index = (base_index + i) % win->window_size;
                                if (win->packets[check_index].seq_num == win->base &&
                                    win->packets[check_index].data != NULL) {
                                    base_pkt = &win->packets[check_index];
                                    break;
                                }
                            }
                        }
                        
                        // Retransmit the packet if found
                        if (base_pkt) {
                            DEBUG_PRINT("Retransmitting packet seq: %u due to repeated RRs\n", win->base);
                            pdu_header_t *pkt_header = (pdu_header_t *)base_pkt->data;
                            pkt_header->flag = FLAG_RESENT_TIMEOUT;  // Mark as resent
                            pkt_header->checksum = 0;
                            pkt_header->checksum = in_cksum((unsigned short *)base_pkt->data, base_pkt->len);
                            safeSendto(sockfd, base_pkt->data, base_pkt->len, 0,
                                       (struct sockaddr *)&client_addr, client_addr_len);
                        } else {
                            DEBUG_PRINT("ERROR: Could not find packet seq %u to resend after repeated RRs\n", win->base);
                        }
                        repeat_rr_count = 0;  // Reset counter after handling
                    }
                } 
                // If we get a new RR, reset the counter
                else if (ack_seq != last_rr_seq) {
                    last_rr_seq = ack_seq;
                    repeat_rr_count = 1;
                }
                
                // Mark packets as acknowledged and slide window if possible
                window_mark_ack(win, ack_seq);
                window_slide(win);
                DEBUG_PRINT("After RR for seq: %u, window base is now: %u\n", ack_seq, win->base);
                DEBUG_PRINT("Window state after ACK: base=%u, packets acknowledged=%u\n", 
                          win->base, ack_seq - win->base + 1);
            } 
            // Process SREJ (Selective Reject) - request to retransmit a specific packet
            else if (ack_header->flag == FLAG_SREJ) {
                // Extract sequence number from packet
                uint32_t srej_seq;
                memcpy(&srej_seq, ack_buf + HEADER_SIZE, 4);
                srej_seq = ntohl(srej_seq);  // Convert from network to host byte order
                
                DEBUG_PRINT("NACK: Received SREJ for seq=%u\n", srej_seq);
                
                // Find the requested packet in the window
                packet_t *pkt = window_get_packet(win, srej_seq);
                if (pkt) {
                    // Prepare and retransmit the packet
                    pdu_header_t *pkt_header = (pdu_header_t *)pkt->data;
                    pkt_header->flag = FLAG_RESENT_SREJ;  // Mark as resent due to SREJ
                    pkt_header->checksum = 0;
                    pkt_header->checksum = in_cksum((unsigned short *)pkt->data, pkt->len);
                    safeSendto(sockfd, pkt->data, pkt->len, 0,
                               (struct sockaddr *)&client_addr, client_addr_len);
                    DEBUG_PRINT("RESEND: Packet seq=%u (in response to SREJ)\n", srej_seq);
                } else {
                    DEBUG_PRINT("ERROR: Could not find packet seq %u to resend\n", srej_seq);
                }
            }
        }
        
        // Check if there are more packets to process
        poll_result = pollCall(0);
    }
}

/* Helper function to handle timeout and retransmit the base packet */
static void handle_timeout(int sockfd, window_t *win, struct sockaddr_in6 client_addr, int client_addr_len,
                           circular_buffer_t *cb, uint32_t seq_num, uint32_t window_size,
                           uint32_t buffer_size, int *active, int eof_reached, int *timeout_counter) {
    uint32_t base = win->base;  // Current window base
    DEBUG_PRINT("TIMEOUT: No acknowledgment received, attempting recovery for seq=%u\n", base);
    packet_t *pkt = NULL;
    int index = base % win->window_size;  // Expected index for base packet
    
    // Try to find the packet at the expected index
    if (win->packets[index].seq_num == base && win->packets[index].data != NULL) {
        pkt = &win->packets[index];
        DEBUG_PRINT("Found packet seq %u at correct index %d\n", base, index);
    } 
    // If not at expected index, search for it or create a new one
    else {
        DEBUG_PRINT("ERROR: Packet at index %d has seq %u, expected %u\n",
               index, win->packets[index].seq_num, base);
        
        // Search through window for the packet
        for (uint32_t i = 0; i < window_size; i++) {
            uint32_t check_seq = base + i;
            if (check_seq >= seq_num) break;  // Don't go beyond what we've sent
            
            int check_index = check_seq % win->window_size;
            if (win->packets[check_index].seq_num == check_seq &&
                win->packets[check_index].data != NULL &&
                !win->packets[check_index].acknowledged) {
                DEBUG_PRINT("Found alternate packet seq %u at index %d\n", check_seq, check_index);
                pkt = &win->packets[check_index];
                break;
            }
        }
        
        // If packet not found, try to recreate it from the circular buffer
        if (!pkt) {
            DEBUG_PRINT("WARNING: Creating new packet for seq %u as it couldn't be found\n", base);
            char data_buf[MAX_DATA_SIZE];
            
            // Read data from circular buffer for this sequence number
            int bytes_read = circular_buffer_read_seq(cb, data_buf, buffer_size, base);
            if (bytes_read > 0) {
                int pdu_len = HEADER_SIZE + bytes_read;
                char *new_pdu = (char *)malloc(pdu_len);
                
                if (new_pdu) {
                    // Create a new packet with the data
                    pdu_header_t header;
                    header.seq_num = htonl(base);
                    header.flag = FLAG_RESENT_TIMEOUT;  // Mark as resent due to timeout
                    header.checksum = 0;
                    
                    // Assemble the packet
                    memcpy(new_pdu, &header, HEADER_SIZE);
                    memcpy(new_pdu + HEADER_SIZE, data_buf, bytes_read);
                    
                    // Calculate and update checksum
                    header.checksum = in_cksum((unsigned short *)new_pdu, pdu_len);
                    memcpy(new_pdu, &header, HEADER_SIZE);
                    
                    // Add to window and send
                    window_add_packet(win, base, new_pdu, pdu_len, FLAG_RESENT_TIMEOUT);
                    DEBUG_PRINT("Created and added new packet seq %u to window\n", base);
                    safeSendto(sockfd, new_pdu, pdu_len, 0,
                               (struct sockaddr *)&client_addr, client_addr_len);
                    DEBUG_PRINT("Directly sent recreated packet seq: %u\n", base);
                    free(new_pdu);
                    return;
                }
            }
        }
    }
    
    // If we found the packet, retransmit it
    if (pkt) {
        DEBUG_PRINT("RESEND: Packet seq=%u (due to timeout, attempt=%d)\n", 
                  pkt->seq_num, pkt->retransmit_count + 1);
        
        // Update packet header for retransmission
        pdu_header_t *pkt_header = (pdu_header_t *)pkt->data;
        pkt_header->flag = FLAG_RESENT_TIMEOUT;  // Mark as resent due to timeout
        pkt_header->checksum = 0;
        pkt_header->checksum = in_cksum((unsigned short *)pkt->data, pkt->len);
        
        // Send the packet
        safeSendto(sockfd, pkt->data, pkt->len, 0,
                   (struct sockaddr *)&client_addr, client_addr_len);
        
        // Track retransmission attempts
        pkt->retransmit_count++;
        
        // If we've retried too many times, skip this packet and move on
        if (pkt->retransmit_count >= MAX_RETRANSMIT) {
            DEBUG_PRINT("SKIP: Packet seq=%u exceeded maximum retransmission attempts (%d)\n", 
                      pkt->seq_num, MAX_RETRANSMIT);
            pkt->acknowledged = true;  // Mark as acknowledged to force window slide
            window_slide(win);
            DEBUG_PRINT("Forced window slide. New base: %u\n", win->base);
            
            // Check if we're done with the file
            if (eof_reached && win->base >= seq_num) {
                DEBUG_PRINT("All sent packets are now acknowledged or skipped. Sending EOF.\n");
                *active = 0;  // End the transfer
            }
            if (win->base < seq_num) return;
        }
    } 
    // If we couldn't find or recreate the packet
    else {
        DEBUG_PRINT("ERROR: Could not find any packet to retransmit. Window may be corrupted.\n");
        
        // After too many timeouts, force the window to move
        if (*timeout_counter > 10) {
            DEBUG_PRINT("Too many consecutive timeouts. Forcing window slide.\n");
            int idx = win->base % win->window_size;
            win->packets[idx].acknowledged = true;  // Mark as acknowledged to force window slide
            window_slide(win);
            DEBUG_PRINT("Forced window slide. New base: %u\n", win->base);
            
            // Check if we're done with the file
            if (eof_reached && win->base >= seq_num) {
                DEBUG_PRINT("All sent packets are now acknowledged or skipped after forced slide. Sending EOF.\n");
                *active = 0;  // End the transfer
            }
        }
    }
}

/* Helper function to send an EOF packet and wait for a final acknowledgment */
static void send_eof_packet(int sockfd, struct sockaddr_in6 client_addr, int client_addr_len,
                            window_t *win, uint32_t seq_num) {
    // Prepare EOF packet
    int eof_pdu_len = HEADER_SIZE;
    char eof_pdu[HEADER_SIZE];
    pdu_header_t eof_header;
    
    // Set up header for EOF packet
    eof_header.seq_num = htonl(seq_num);  // Use next sequence number
    eof_header.flag    = FLAG_EOF;        // Mark as EOF
    eof_header.checksum = 0;
    
    // Assemble the packet
    memcpy(eof_pdu, &eof_header, HEADER_SIZE);
    
    // Calculate and update checksum
    eof_header.checksum = in_cksum((unsigned short *)eof_pdu, eof_pdu_len);
    memcpy(eof_pdu, &eof_header, HEADER_SIZE);
    
    // Log transfer summary
    DEBUG_PRINT("=== SENDING EOF PACKET ===\n");
    DEBUG_PRINT("Total packets sent: %u, Last sequence number: %u\n", seq_num, seq_num-1);
    DEBUG_PRINT("Window base: %u, Unacknowledged packets: %u\n", 
              win->base, seq_num > win->base ? seq_num - win->base : 0);
    
    int eof_retries = 0;
    int final_ack_received = 0;
    
    // Retry loop for sending EOF and waiting for final acknowledgment
    while (eof_retries < MAX_RETRANSMIT && !final_ack_received) {
        DEBUG_PRINT("SEND EOF packet seq=%u (attempt %d/%d)\n", 
                  seq_num, eof_retries+1, MAX_RETRANSMIT);
        
        // Send the EOF packet
        safeSendto(sockfd, eof_pdu, eof_pdu_len, 0,
                   (struct sockaddr *)&client_addr, client_addr_len);
        
        // Wait for response with 1 second timeout
        int poll_result_final = pollCall(1000);
        
        if (poll_result_final > 0) {
            DEBUG_PRINT("Received a response after sending EOF\n");
            
            // Receive the packet
            char final_ack[HEADER_SIZE + 4];
            struct sockaddr_in6 recv_addr;
            int recv_addr_len = sizeof(recv_addr);
            int recv_len = safeRecvfrom(sockfd, final_ack, sizeof(final_ack), 0,
                                        (struct sockaddr *)&recv_addr, &recv_addr_len);
            
            // Process if packet is valid
            if (recv_len >= HEADER_SIZE + 4) {
                pdu_header_t *ack_header = (pdu_header_t *)final_ack;
                
                // Verify checksum
                uint16_t recv_chk = ack_header->checksum;
                ack_header->checksum = 0;
                uint16_t calc_chk = in_cksum((unsigned short *)final_ack, recv_len);
                
                if (recv_chk != calc_chk) {
                    DEBUG_PRINT("Received corrupted final ack. Ignoring.\n");
                } 
                // Process RR (Ready to Receive) acknowledgment
                else if (ack_header->flag == FLAG_RR) {
                    // Extract sequence number
                    uint32_t ack_seq;
                    memcpy(&ack_seq, final_ack + HEADER_SIZE, 4);
                    ack_seq = ntohl(ack_seq);
                    
                    // Accept if it's for the right sequence or we've tried enough times
                    if (ack_seq >= win->base - 1 || eof_retries >= 3) {
                        DEBUG_PRINT("=== TRANSFER COMPLETE ===\n");
                        DEBUG_PRINT("Final acknowledgment received (seq=%u)\n", ack_seq);
                        final_ack_received = 1;
                        break;
                    } else {
                        DEBUG_PRINT("Received ack for seq %u but waiting for newer ack (window base: %u)\n",
                               ack_seq, win->base);
                    }
                } 
                // Process SREJ (Selective Reject)
                else if (ack_header->flag == FLAG_SREJ) {
                    // Extract sequence number
                    uint32_t srej_seq;
                    memcpy(&srej_seq, final_ack + HEADER_SIZE, 4);
                    srej_seq = ntohl(srej_seq);
                    
                    DEBUG_PRINT("Client sent SREJ for seq %u even after EOF. ", srej_seq);
                    if (srej_seq < win->base) {
                        DEBUG_PRINT("This packet was skipped.\n");
                    } else {
                        DEBUG_PRINT("Ignoring.\n");
                    }
                    
                    // After enough retries, accept any response as final
                    if (eof_retries >= 3) {
                        DEBUG_PRINT("Accepting client response as final acknowledgment after %d EOF attempts.\n", eof_retries);
                        final_ack_received = 1;
                        break;
                    }
                    continue;
                }
            }
        } else {
            DEBUG_PRINT("No response received for EOF packet within timeout\n");
        }
        
        // Increment retry counter
        eof_retries++;
        
        // After enough retries, consider transfer complete anyway
        if (eof_retries >= 5) {
            DEBUG_PRINT("Considering transfer complete after %d EOF attempts with some client response.\n", eof_retries);
            final_ack_received = 1;
            break;
        }
    }
    
    if (!final_ack_received) {
        DEBUG_PRINT("Final acknowledgment not received after %d attempts. Exiting session.\n", MAX_RETRANSMIT);
    }
}

/* Send file data packets using sliding window with selective-reject ARQ */
void send_data_packets(int sockfd, struct sockaddr_in6 client_addr, int client_addr_len,
                       FILE *fp, uint32_t window_size, uint32_t buffer_size, double error_rate) {
    // Initialize sliding window and circular buffer for data management
    window_t *win = window_init(window_size);
    uint32_t seq_num = 0;  // Next sequence number to send
    int eof_reached = 0;   // Flag for end of file
    int active = 1;        // Flag for active transfer
    
    // Create circular buffer with twice the capacity needed for the window
    size_t cb_size = 2 * window_size * buffer_size;
    circular_buffer_t *cb = circular_buffer_init(cb_size, buffer_size);
    
    // Set up polling for socket
    setupPollSet();
    addToPollSet(sockfd);
    
    // Variables for timeout handling
    struct timeval last_activity;
    int timeout_counter = 0;
    uint32_t last_base = 0;
    
    // Record start time
    gettimeofday(&last_activity, NULL);
    
    // Log transfer start
    DEBUG_PRINT("=== STARTING FILE TRANSFER SESSION ===\n");
    DEBUG_PRINT("Window size: %u packets, Buffer size: %u bytes\n", window_size, buffer_size);
    DEBUG_PRINT("Created sliding window with %u slots\n", window_size);
    DEBUG_PRINT("Created circular buffer with capacity: %zu bytes\n", cb_size);

    // Main transfer loop
    while (active) {
        /* Fill the window with new data if possible */
        while (((seq_num - win->base) < window_size) && !eof_reached) {
            DEBUG_PRINT("Window status: base=%u, next_seq=%u, available slots=%u\n", 
                      win->base, seq_num, window_size - (seq_num - win->base));
            
            // Read data from file
            char data_buf[MAX_DATA_SIZE];
            int bytes_read = fread(data_buf, 1, buffer_size, fp);
            
            // Check for end of file
            if (bytes_read <= 0) {
                eof_reached = 1;
                DEBUG_PRINT("End of file reached.\n");
                break;
            }
            
            // Store data in circular buffer for potential retransmissions
            circular_buffer_write(cb, data_buf, bytes_read, seq_num);
            
            // Create packet with data
            int pdu_len = HEADER_SIZE + bytes_read;
            char *pdu = (char *)malloc(pdu_len);
            if (!pdu) { 
                perror("malloc"); 
                exit(1); 
            }
            
            // Set up header
            pdu_header_t header;
            header.seq_num = htonl(seq_num);  // Network byte order
            header.flag    = FLAG_DATA;       // Data packet
            header.checksum = 0;              // Initialize checksum
            
            // Assemble packet
            memcpy(pdu, &header, HEADER_SIZE);
            memcpy(pdu + HEADER_SIZE, data_buf, bytes_read);
            
            // Calculate and update checksum
            header.checksum = in_cksum((unsigned short *)pdu, pdu_len);
            memcpy(pdu, &header, HEADER_SIZE);
            
            // Add to window and send
            window_add_packet(win, seq_num, pdu, pdu_len, FLAG_DATA);
            safeSendto(sockfd, pdu, pdu_len, 0,
                       (struct sockaddr *)&client_addr, client_addr_len);
            DEBUG_PRINT("SEND DATA packet seq=%u, size=%d bytes, flag=%d\n", 
                      seq_num, pdu_len, FLAG_DATA);
            
            // Free allocated memory
            free(pdu);
            
            // Process any acknowledgments that arrived
            process_ack_packets(sockfd, win, client_addr, client_addr_len);
            
            // Move to next sequence number
            seq_num++;
        }

        // Check if transfer is complete
        if (eof_reached && (win->base == seq_num)) {
            active = 0;
            break;
        }

        // Set timeout based on window state
        int timeout = (((seq_num - win->base) == window_size) ? 1000 : 0);
        int should_handle_timeout = 0;
        
        // If window is full, wait for acknowledgments
        if ((seq_num - win->base) == window_size) {
            DEBUG_PRINT("Window FULL [%u-%u]. Waiting for acknowledgments...\n", 
                      win->base, win->base + window_size - 1);
            
            // Check if window base hasn't moved
            if (win->base == last_base) {
                timeout_counter++;
                if (timeout_counter >= 3) {
                    DEBUG_PRINT("Forced timeout: window base hasn't moved for %d iterations\n", timeout_counter);
                    should_handle_timeout = 1;
                }
            } else {
                timeout_counter = 0;
                last_base = win->base;
            }
        }
        
        // Handle normal packet processing or timeout
        if (!should_handle_timeout) {
            // Wait for incoming packets
            int poll_result = pollCall(timeout);
            
            if (poll_result > 0) {
                // Reset timeout counter on activity
                timeout_counter = 0;
                gettimeofday(&last_activity, NULL);
                
                // Receive the packet
                char ack_buf[HEADER_SIZE + 4];
                struct sockaddr_in6 recv_addr;
                int recv_addr_len = sizeof(recv_addr);
                int recv_len = safeRecvfrom(sockfd, ack_buf, sizeof(ack_buf), 0,
                                            (struct sockaddr *)&recv_addr, &recv_addr_len);
                
                // Process if packet is valid
                if (recv_len >= HEADER_SIZE + 4) {
                    pdu_header_t *ack_header = (pdu_header_t *)ack_buf;
                    
                    // Verify checksum
                    uint16_t recv_chk = ack_header->checksum;
                    ack_header->checksum = 0;
                    uint16_t calc_chk = in_cksum((unsigned short *)ack_buf, recv_len);
                    
                    if (recv_chk != calc_chk) {
                        DEBUG_PRINT("Received corrupted ack packet. Ignoring.\n");
                    } else if (ack_header->flag == FLAG_RR) {
                        // Extract the sequence number from the acknowledgment
                        uint32_t ack_seq;
                        memcpy(&ack_seq, ack_buf + HEADER_SIZE, 4);
                        ack_seq = ntohl(ack_seq);
                        DEBUG_PRINT("Received RR for seq: %u\n", ack_seq);
                        
                        // Track repeated RRs to detect potential packet loss
                        static uint32_t last_rr_seq = 0;
                        static int repeat_rr_count = 0;
                        
                        // If we're getting the same RR repeatedly for the packet just before our window base
                        // it likely means the client is missing the packet at our window base
                        if (last_rr_seq == ack_seq && ack_seq == win->base - 1) {
                            repeat_rr_count++;
                            DEBUG_PRINT("Detected repeated RR for seq: %u (%d times)\n", ack_seq, repeat_rr_count);
                            
                            // After receiving the same RR 3 times, assume packet loss
                            if (repeat_rr_count >= 3) {
                                DEBUG_PRINT("Client appears to be missing packet at window base (%u)\n", win->base);
                                
                                // Try to find the packet at the window base
                                int base_index = win->base % win->window_size;
                                packet_t *base_pkt = NULL;
                                
                                // First check the expected index
                                if (win->packets[base_index].seq_num == win->base &&
                                    win->packets[base_index].data != NULL) {
                                    base_pkt = &win->packets[base_index];
                                } else {
                                    // If not at expected index, search the entire window
                                    for (int i = 0; i < win->window_size; i++) {
                                        int check_index = (base_index + i) % win->window_size;
                                        if (win->packets[check_index].seq_num == win->base &&
                                            win->packets[check_index].data != NULL) {
                                            base_pkt = &win->packets[check_index];
                                            break;
                                        }
                                    }
                                }
                                
                                // If we found the packet, retransmit it
                                if (base_pkt) {
                                    DEBUG_PRINT("Retransmitting packet seq: %u due to repeated RRs\n", win->base);
                                    pdu_header_t *pkt_header = (pdu_header_t *)base_pkt->data;
                                    pkt_header->flag = FLAG_RESENT_TIMEOUT;  // Mark as resent
                                    pkt_header->checksum = 0;
                                    pkt_header->checksum = in_cksum((unsigned short *)base_pkt->data, base_pkt->len);
                                    safeSendto(sockfd, base_pkt->data, base_pkt->len, 0,
                                               (struct sockaddr *)&client_addr, client_addr_len);
                                } else {
                                    DEBUG_PRINT("ERROR: Could not find packet seq %u to resend after repeated RRs\n", win->base);
                                }
                                repeat_rr_count = 0;  // Reset counter after handling
                            }
                        } else if (ack_seq != last_rr_seq) {
                            // New RR received, reset counter
                            last_rr_seq = ack_seq;
                            repeat_rr_count = 1;
                        }
                        
                        // Process the acknowledgment
                        window_mark_ack(win, ack_seq);
                        window_slide(win);
                        DEBUG_PRINT("After RR for seq: %u, window base is now: %u\n", ack_seq, win->base);
                    } else if (ack_header->flag == FLAG_SREJ) {
                        // Handle selective reject - client is requesting a specific packet
                        uint32_t srej_seq;
                        memcpy(&srej_seq, ack_buf + HEADER_SIZE, 4);
                        srej_seq = ntohl(srej_seq);
                        DEBUG_PRINT("Received SREJ for seq: %u. Resending packet.\n", srej_seq);
                        
                        // Find the requested packet in our window
                        packet_t *pkt = window_get_packet(win, srej_seq);
                        if (pkt) {
                            // Update header and retransmit
                            pdu_header_t *pkt_header = (pdu_header_t *)pkt->data;
                            pkt_header->flag = FLAG_RESENT_SREJ;  // Mark as resent due to SREJ
                            pkt_header->checksum = 0;
                            pkt_header->checksum = in_cksum((unsigned short *)pkt->data, pkt->len);
                            safeSendto(sockfd, pkt->data, pkt->len, 0,
                                       (struct sockaddr *)&client_addr, client_addr_len);
                        } else {
                            DEBUG_PRINT("ERROR: Could not find packet seq %u to resend\n", srej_seq);
                        }
                    }
                }
            } else if (poll_result == 0 && ((seq_num - win->base) == window_size)) {
                // Timeout occurred while window is full
                should_handle_timeout = 1;
            }
        }
        
        if (should_handle_timeout) {
            // Handle timeout - will retransmit packets as needed
            handle_timeout(sockfd, win, client_addr, client_addr_len, cb, seq_num,
                           window_size, buffer_size, &active, eof_reached, &timeout_counter);
        }
        
        // Check if we're done with the file
        if (eof_reached && win->base >= seq_num) {
            DEBUG_PRINT("All packets are either acknowledged or skipped after end of file. Sending EOF.\n");
            active = 0;
        }
    }
    
    // Send final EOF packet and clean up
    send_eof_packet(sockfd, client_addr, client_addr_len, win, seq_num);
    window_free(win);
    circular_buffer_free(cb);
    DEBUG_PRINT("=== ENDING FILE TRANSFER SESSION ===\n");
}
