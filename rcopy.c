#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <arpa/inet.h>
#include <unistd.h>
#include "protocol.h"
#include "window.h"
#include "gethostbyname.h"
#include "networks.h"
#include "safeUtil.h"
#include "pollLib.h"
#include "cpe464.h"
#include "checksum.h"
#include "debug.h"


// Sends the initial filename request to the server
// This is the first step in our file transfer process
static void send_filename_request(int sockfd, struct sockaddr_in6 *server_addr,
                                  const char *from_filename, int window_size, int buffer_size);

// Sends a Receiver Ready (RR) packet to acknowledge data
static void send_rr_packet(int sockfd, struct sockaddr_in6 *server_addr, uint32_t seq);

// Sends a Selective Reject (SREJ) packet to request a specific missing packet
static void send_srej_packet(int sockfd, struct sockaddr_in6 *server_addr, uint32_t seq);

// Sends multiple RR packets to make sure the server gets our final acknowledgment
static void send_final_rr(int sockfd, struct sockaddr_in6 *server_addr, uint32_t seq, int times);

// The main function that handles receiving file data and sending acknowledgments
static void process_file_transfer(int sockfd, struct sockaddr_in6 *server_addr, FILE *outfile, int window_size);

int main(int argc, char *argv[]) {
    /* Initialize debug mode */
    debug_init(argc, argv);
    
    /* Add a debug header to show client startup */
    DEBUG_PRINT("\n====== RCOPY CLIENT STARTING ======\n");
    
    /* Check for required arguments, ignoring debug flag */
    int required_args = 7;
    int provided_args = argc;
    if (argc > 1 && strcmp(argv[argc-1], "-d") == 0) {
        provided_args--;
    }
    
    if (provided_args != required_args + 1) {
        fprintf(stderr, "Usage: %s from-filename to-filename window-size buffer-size error-rate remote-machine remote-port [-d]\n", argv[0]);
        exit(1);
    }
    
    /* Parse arguments */
    char *from_filename = argv[1];
    char *to_filename   = argv[2];
    int   window_size   = atoi(argv[3]);
    int   buffer_size   = atoi(argv[4]);
    double error_rate   = atof(argv[5]);
    char *remote_machine= argv[6];
    int   remote_port   = atoi(argv[7]);
    
    /* Validate inputs */
    if (strlen(from_filename) > 100) {
        fprintf(stderr, "Error: file %s name too long (max 100 chars).\n", from_filename);
        exit(1);
    }
    
    if (window_size <= 0 || window_size >= (1 << 30)) {
        fprintf(stderr, "Error: invalid window size %d (must be > 0 and < 2^30).\n", window_size);
        exit(1);
    }
    
    if (buffer_size <= 0 || buffer_size > MAX_DATA_SIZE) {
        fprintf(stderr, "Error: invalid buffer size %d (must be > 0 and <= %d).\n", 
                buffer_size, MAX_DATA_SIZE);
        exit(1);
    }
    
    DEBUG_PRINT("Starting rcopy file transfer:\n");
    DEBUG_PRINT("  Source file: %s\n", from_filename);
    DEBUG_PRINT("  Destination file: %s\n", to_filename);
    DEBUG_PRINT("  Window size: %d packets\n", window_size);
    DEBUG_PRINT("  Buffer size: %d bytes\n", buffer_size);
    DEBUG_PRINT("  Error rate: %.2f\n", error_rate);
    DEBUG_PRINT("  Remote host: %s:%d\n", remote_machine, remote_port);
    
    /* Open output file */
    FILE *outfile = fopen(to_filename, "wb");
    if (!outfile) {
        fprintf(stderr, "Error on open of output file: %s\n", to_filename);
        exit(1);
    }
    
    /* Setup UDP client socket using networks.h */
    DEBUG_PRINT("Setting up UDP client socket...\n");
    struct sockaddr_in6 server_addr;
    int sockfd = setupUdpClientToServer(&server_addr, remote_machine, remote_port);
    if (sockfd < 0) {
        fprintf(stderr, "Error setting up UDP client socket\n");
        fclose(outfile);
        exit(1);
    }
    
    /* Get local address information */
    struct sockaddr_in6 local_addr;
    socklen_t addr_len = sizeof(local_addr);
    getsockname(sockfd, (struct sockaddr *)&local_addr, &addr_len);
    
    DEBUG_PRINT("Client socket created successfully\n");
    DEBUG_PRINT("Local client port: %d\n", ntohs(local_addr.sin6_port));
    DEBUG_PRINT("Server address: %s:%d\n", 
           ipAddressToString(&server_addr), ntohs(server_addr.sin6_port));
    
    setupPollSet();
    addToPollSet(sockfd);
    
    /* Initialize error injection for sends */
    DEBUG_PRINT("Initializing packet error injection (error rate: %.2f)\n", error_rate);
    sendtoErr_init(error_rate, DROP_ON, FLIP_ON, DEBUG_OFF, RSEED_ON);
    
    /* Send the initial filename request and wait for OK response */
    DEBUG_PRINT("\n=== INITIATING CONNECTION ===\n");
    send_filename_request(sockfd, &server_addr, from_filename, window_size, buffer_size);
    
    /* Process incoming file data packets and write to output file */
    DEBUG_PRINT("\n=== STARTING FILE TRANSFER ===\n");
    process_file_transfer(sockfd, &server_addr, outfile, window_size);
    
    DEBUG_PRINT("\n=== FILE TRANSFER COMPLETE ===\n");
    DEBUG_PRINT("Closing output file: %s\n", to_filename);
    fclose(outfile);
    close(sockfd);
    DEBUG_PRINT("====== RCOPY CLIENT FINISHED ======\n");
    return 0;
}

/* --- Helper Function Implementations --- */

/* Send the initial filename request (FLAG_FILENAME) and wait for a valid response */
static void send_filename_request(int sockfd, struct sockaddr_in6 *server_addr,
                                  const char *from_filename, int window_size, int buffer_size) {
    /* Create and initialize the payload structure with filename and parameters */
    init_payload_t init_payload;
    memset(&init_payload, 0, sizeof(init_payload));
    strncpy(init_payload.filename, from_filename, 100);
    init_payload.window_size = htonl(window_size);  /* Convert to network byte order */
    init_payload.buffer_size = htonl(buffer_size);  /* Convert to network byte order */
    
    /* Calculate sizes for the PDU (Protocol Data Unit) */
    int payload_len = sizeof(init_payload);
    int pdu_len = HEADER_SIZE + payload_len;
    
    /* Allocate memory for the complete PDU */
    char *pdu = (char *)malloc(pdu_len);
    if (!pdu) {
        perror("malloc");
        fclose(NULL);
        close(sockfd);
        exit(1);
    }
    
    /* Create and initialize the PDU header */
    pdu_header_t header;
    header.seq_num = htonl(0);        /* First packet has sequence number 0 */
    header.flag    = FLAG_FILENAME;   /* Set flag to indicate filename request */
    header.checksum = 0;              /* Initialize checksum to 0 before calculation */
    
    /* Copy header and payload into the PDU buffer */
    memcpy(pdu, &header, HEADER_SIZE);
    memcpy(pdu + HEADER_SIZE, &init_payload, payload_len);
    
    /* Calculate checksum over the entire PDU */
    header.checksum = in_cksum((unsigned short *)pdu, pdu_len);
    
    /* Update the PDU with the calculated checksum */
    memcpy(pdu, &header, HEADER_SIZE);
    
    /* Log details about the request packet */
    DEBUG_PRINT("Building filename request packet\n");
    DEBUG_PRINT("  Requested file: %s\n", from_filename);
    DEBUG_PRINT("  Negotiated window size: %d\n", window_size);
    DEBUG_PRINT("  Negotiated buffer size: %d\n", buffer_size);
    
    DEBUG_PRINT("Sending filename request to server\n");
    
    /* Retry loop for sending the filename request */
    int retries = 0;
    int received = 0;
    while (retries < INIT_RETRY_LIMIT && !received) {
        DEBUG_PRINT("SEND FILENAME request (attempt %d/%d)\n", retries+1, INIT_RETRY_LIMIT);
        
        /* Send the request packet to the server */
        safeSendto(sockfd, pdu, pdu_len, 0, (struct sockaddr *)server_addr, sizeof(*server_addr));
        
        DEBUG_PRINT("Waiting for server response (timeout: 5 seconds)...\n");
        
        /* Wait for a response with timeout */
        if (pollCall(5000) > 0) {  // wait up to 5 sec
            DEBUG_PRINT("Activity detected on socket\n");
            
            /* Prepare to receive the response */
            char resp_buf[1024];
            struct sockaddr_in6 resp_addr;
            int addrlen = sizeof(resp_addr);
            
            /* Receive data from the socket */
            int recv_len = safeRecvfrom(sockfd, resp_buf, sizeof(resp_buf), 0,
                                        (struct sockaddr *)&resp_addr, &addrlen);
            
            /* Process the received packet if it's at least as large as a header */
            if (recv_len >= HEADER_SIZE) {
                /* Extract and validate the header */
                pdu_header_t *resp_header = (pdu_header_t *)resp_buf;
                uint16_t recv_chk = resp_header->checksum;
                resp_header->checksum = 0;  /* Zero out for checksum calculation */
                uint16_t calc_chk = in_cksum((unsigned short *)resp_buf, recv_len);
                uint32_t packet_seq = ntohl(resp_header->seq_num);
                
                DEBUG_PRINT("RECV packet: seq=%u, flag=%d, size=%d, expected=%u\n", 
                       packet_seq, resp_header->flag, recv_len, 0);
                   
                /* Verify packet integrity using checksum */
                if (recv_chk != calc_chk) {
                    DEBUG_PRINT("ERROR: Corrupted response packet (checksum mismatch)\n");
                    DEBUG_PRINT("  Received checksum: 0x%04x\n", recv_chk);
                    DEBUG_PRINT("  Calculated checksum: 0x%04x\n", calc_chk);
                    retries++;
                    continue;
                }
                
                /* Check if this is the expected response type */
                if (resp_header->flag == FLAG_FILENAME_RESP) {
                    /* Check payload – if not "OK", file not found */
                    if (recv_len > HEADER_SIZE) {
                        char response[256];
                        memcpy(response, resp_buf + HEADER_SIZE, recv_len - HEADER_SIZE);
                        response[recv_len - HEADER_SIZE] = '\0';  /* Null-terminate the string */
                        DEBUG_PRINT("SERVER RESPONSE: \"%s\"\n", response);
                        
                        /* Check if the file exists on the server */
                        if (strcmp(response, "OK") != 0) {
                            fprintf(stderr, "Error: file %s not found.\n", from_filename);
                            free(pdu);
                            exit(1);
                        } else {
                            DEBUG_PRINT("File exists on server and is ready for transfer\n");
                        }
                    }
                    
                    /* Update server address with the source of the response */
                    DEBUG_PRINT("Updating server address from response\n");
                    DEBUG_PRINT("Previous server info - IP: %s Port: %d\n", 
                            ipAddressToString(server_addr), ntohs(server_addr->sin6_port));
                    memcpy(server_addr, &resp_addr, sizeof(resp_addr));
                    DEBUG_PRINT("Updated server info - IP: %s Port: %d\n", 
                            ipAddressToString(server_addr), ntohs(server_addr->sin6_port));
                    received = 1;  /* Mark as received successfully */
                    break;
                } else {
                    /* Unexpected packet type */
                    DEBUG_PRINT("Received unexpected packet type (flag=%d), ignoring\n", resp_header->flag);
                }
            } else {
                /* Packet too small or timeout occurred */
                DEBUG_PRINT("Timeout waiting for server response\n");
            }
            retries++;
        }
        
        /* Check if we've exhausted all retries */
        if (!received) {
            fprintf(stderr, "Failed to initialize file transfer after %d retries\n", INIT_RETRY_LIMIT);
            free(pdu);
            exit(1);
        }
    }
    
    DEBUG_PRINT("Connection with server established successfully\n");
    free(pdu);
}

/* Helper to send a Receiver Ready (RR) packet for a given sequence number */
static void send_rr_packet(int sockfd, struct sockaddr_in6 *server_addr, uint32_t seq) {
    /* Create a buffer for the Receiver Ready (RR) packet */
    char rr_pdu[HEADER_SIZE + 4];
    
    /* Initialize the packet header */
    pdu_header_t rr_header;
    rr_header.seq_num = htonl(seq);    /* Convert sequence number to network byte order */
    rr_header.flag    = FLAG_RR;       /* Set flag to indicate this is a Receiver Ready packet */
    rr_header.checksum = 0;            /* Initialize checksum to 0 before calculation */
    
    /* Copy header to the PDU buffer */
    memcpy(rr_pdu, &rr_header, HEADER_SIZE);
    
    /* Copy sequence number to the payload section (needed for verification) */
    memcpy(rr_pdu + HEADER_SIZE, &rr_header.seq_num, 4);
    
    /* Calculate checksum over the entire packet */
    rr_header.checksum = in_cksum((unsigned short *)rr_pdu, HEADER_SIZE+4);
    
    /* Copy updated header with checksum back to the PDU buffer */
    memcpy(rr_pdu, &rr_header, HEADER_SIZE);
    
    /* Log the acknowledgment if debug mode is enabled */
    DEBUG_PRINT("SEND ACK (RR) for seq=%u\n", seq);
    
    /* Send the packet to the server using error-simulating send function */
    safeSendto(sockfd, rr_pdu, HEADER_SIZE+4, 0, (struct sockaddr *)server_addr, sizeof(*server_addr));
}

/* Helper to send a Selective Reject (SREJ) packet for a given sequence number */
static void send_srej_packet(int sockfd, struct sockaddr_in6 *server_addr, uint32_t seq) {
    /* Create a buffer for the Selective Reject (SREJ) packet */
    char srej_pdu[HEADER_SIZE + 4];
    
    /* Initialize the packet header */
    pdu_header_t srej_header;
    srej_header.seq_num = htonl(seq);    /* Convert sequence number to network byte order */
    srej_header.flag    = FLAG_SREJ;     /* Set flag to indicate this is a Selective Reject packet */
    srej_header.checksum = 0;            /* Initialize checksum to 0 before calculation */
    
    /* Copy header to the PDU buffer */
    memcpy(srej_pdu, &srej_header, HEADER_SIZE);
    
    /* Copy sequence number to the payload section (needed for verification) */
    memcpy(srej_pdu + HEADER_SIZE, &srej_header.seq_num, 4);
    
    /* Calculate checksum over the entire packet */
    srej_header.checksum = in_cksum((unsigned short *)srej_pdu, HEADER_SIZE+4);
    
    /* Copy updated header with checksum back to the PDU buffer */
    memcpy(srej_pdu, &srej_header, HEADER_SIZE);
    
    /* Log the negative acknowledgment if debug mode is enabled */
    DEBUG_PRINT("SEND NACK (SREJ) for seq=%u\n", seq);
    
    /* Send the packet to the server using error-simulating send function */
    safeSendto(sockfd, srej_pdu, HEADER_SIZE+4, 0, (struct sockaddr *)server_addr, sizeof(*server_addr));
}

/* Helper to send the final RR packet multiple times */
static void send_final_rr(int sockfd, struct sockaddr_in6 *server_addr, uint32_t seq, int times) {
    /* Log that we're sending final acknowledgments */
    DEBUG_PRINT("Sending final acknowledgments (RR) to ensure reliable termination\n");
    
    /* Send the RR packet multiple times to increase reliability of termination */
    for (int i = 0; i < times; i++) {
        /* Call the RR packet sending function */
        send_rr_packet(sockfd, server_addr, seq);
        
        /* Log each attempt if debug mode is enabled */
        DEBUG_PRINT("SEND FINAL ACK (RR) for seq=%u (attempt %d/%d)\n", seq, i+1, times);
    }
}

/* Process incoming data packets until the file transfer is complete */
static void process_file_transfer(int sockfd, struct sockaddr_in6 *server_addr, FILE *outfile, int window_size) {
    /* Initialize sequence tracking variables */
    uint32_t expected_seq = 0;          /* Next expected sequence number */
    uint32_t highest_received_seq = 0;   /* Highest sequence number received so far */
    int eof_received = 0;               /* Flag indicating if EOF packet was received */
    int consecutive_timeouts = 0;        /* Count of timeouts without receiving data */
    int finished = 0;                    /* Flag indicating transfer completion */
    
    /* Initialize sliding window for buffering out-of-order packets */
    window_t *win = window_init(window_size);
    win->base = expected_seq;
    
    DEBUG_PRINT("Initialized sliding window with %d slots\n", window_size);
    DEBUG_PRINT("Window base set to %u\n", expected_seq);
    
    /* Buffer for receiving data packets */
    char data_buf[MAX_PDU_SIZE];
    DEBUG_PRINT("Ready to receive data packets, expecting seq=%u\n", expected_seq);
    
    /* Main packet processing loop */
    while (!finished) {
        DEBUG_PRINT("Waiting for data (timeout: %d ms)...\n", DATA_TIMEOUT);
        int poll_result = pollCall(DATA_TIMEOUT);  // wait up to 10 sec for data
        
        if (poll_result > 0) {  /* Data is available to read */
            DEBUG_PRINT("Activity detected on socket\n");
            
            /* Set up to receive packet */
            struct sockaddr_in6 src_addr;
            int addrlen = sizeof(src_addr);
            int recv_len = safeRecvfrom(sockfd, data_buf, sizeof(data_buf), 0,
                                        (struct sockaddr *)&src_addr, &addrlen);
            
            /* Validate packet size */
            if (recv_len < HEADER_SIZE)
                continue;
                
            /* Extract and validate packet header */
            pdu_header_t *recv_header = (pdu_header_t *)data_buf;
            uint16_t recv_chk = recv_header->checksum;
            recv_header->checksum = 0;
            uint16_t calc_chk = in_cksum((unsigned short *)data_buf, recv_len);
            uint32_t packet_seq = ntohl(recv_header->seq_num);
            
            DEBUG_PRINT("RECV packet: seq=%u, flag=%d, size=%d, expected=%u\n", 
                   packet_seq, recv_header->flag, recv_len, expected_seq);
                   
            /* Check for packet corruption */
            if (recv_chk != calc_chk) {
                DEBUG_PRINT("ERROR: Corrupted packet detected (checksum mismatch)\n");
                DEBUG_PRINT("  Received checksum: 0x%04x\n", recv_chk);
                DEBUG_PRINT("  Calculated checksum: 0x%04x\n", calc_chk);
                DEBUG_PRINT("Requesting retransmission of seq=%u\n", expected_seq);
                send_srej_packet(sockfd, server_addr, expected_seq);
                continue;
            }
            
            /* Handle data packets (including retransmissions) */
            if (recv_header->flag == FLAG_DATA ||
                recv_header->flag == FLAG_RESENT_SREJ ||
                recv_header->flag == FLAG_RESENT_TIMEOUT) {
                
                if (packet_seq == expected_seq) {  /* In-order packet */
                    /* Process the expected packet */
                    DEBUG_PRINT("Received expected packet seq=%u\n", packet_seq);
                    int data_len = recv_len - HEADER_SIZE;
                    fwrite(data_buf + HEADER_SIZE, 1, data_len, outfile);
                    DEBUG_PRINT("Wrote %d bytes from packet to output file\n", data_len);
                    send_rr_packet(sockfd, server_addr, packet_seq);
                    expected_seq++;
                    
                    /* Process any buffered packets that are now in order */
                    DEBUG_PRINT("Checking for buffered packets...\n");
                    int buffered_count = 0;
                    while (true) {
                        packet_t *next_packet = window_get_packet(win, expected_seq);
                        if (next_packet) {
                            buffered_count++;
                            int next_data_len = next_packet->len - HEADER_SIZE;
                            fwrite(next_packet->data + HEADER_SIZE, 1, next_data_len, outfile);
                            DEBUG_PRINT("Found buffered packet seq=%u, writing %d bytes to file\n", 
                                   expected_seq, next_data_len);
                            window_mark_ack(win, expected_seq);
                            send_rr_packet(sockfd, server_addr, expected_seq);
                            expected_seq++;
                        } else {
                            break;
                        }
                    }
                    if (buffered_count > 0) {
                        DEBUG_PRINT("Processed %d buffered packets\n", buffered_count);
                    } else {
                        DEBUG_PRINT("No buffered packets found\n");
                    }
                    
                } else if (packet_seq > expected_seq) {
                    /* Out-of-order packet: buffer it and SREJ expected */
                    DEBUG_PRINT("Out-of-order packet: received seq=%u but expected seq=%u\n", 
                           packet_seq, expected_seq);
                    DEBUG_PRINT("Buffering out-of-order packet\n");
                    
                    /* Update window base if needed */
                    if (win->base < expected_seq) {
                        win->base = expected_seq;
                        DEBUG_PRINT("Updated window base to %u\n", win->base);
                    }
                    
                    /* Store packet in buffer */
                    DEBUG_PRINT("Adding packet seq=%u to buffer\n", packet_seq);
                    window_add_packet(win, packet_seq, data_buf, recv_len, recv_header->flag);
                    
                    /* Request missing packet */
                    DEBUG_PRINT("Requesting missing packet seq=%u\n", expected_seq);
                    send_srej_packet(sockfd, server_addr, expected_seq);
                    
                } else {  /* Duplicate or old packet */
                    DEBUG_PRINT("Ignoring duplicate/old packet: received seq=%u but expected seq=%u\n", 
                           packet_seq, expected_seq);
                    if (expected_seq > 0) {
                        DEBUG_PRINT("Re-acknowledging previous packet seq=%u\n", expected_seq - 1);
                        send_rr_packet(sockfd, server_addr, expected_seq - 1);
                    }
                }
                
            } else if (recv_header->flag == FLAG_EOF) {  /* End of file packet */
                /* Process EOF packet */
                DEBUG_PRINT("Received EOF packet (seq=%u)\n", packet_seq);
                int data_len = recv_len - HEADER_SIZE;
                if (data_len > 0) {
                    fwrite(data_buf + HEADER_SIZE, 1, data_len, outfile);
                    DEBUG_PRINT("Wrote final %d bytes from EOF packet to output file\n", data_len);
                }
                DEBUG_PRINT("Sending final acknowledgments\n");
                send_final_rr(sockfd, server_addr, expected_seq > 0 ? expected_seq - 1 : 0, 3);
                eof_received = 1;
                finished = 1;
                DEBUG_PRINT("EOF received, file transfer complete\n");
            }
            
            /* Update highest received sequence if needed */
            if (packet_seq > highest_received_seq) {
                highest_received_seq = packet_seq;
                DEBUG_PRINT("Updated highest received sequence to %u\n", highest_received_seq);
            }
        } else {  /* Timeout occurred */
            /* Handle timeout case */
            DEBUG_PRINT("TIMEOUT: No data received within %d ms\n", DATA_TIMEOUT);
            
            if (eof_received) {
                DEBUG_PRINT("EOF was already received, assuming transfer is complete\n");
                finished = 1;
                break;
            }
            
            /* Send acknowledgment for highest received packet */
            DEBUG_PRINT("Sending acknowledgment for highest received seq=%u\n", highest_received_seq);
            send_rr_packet(sockfd, server_addr, highest_received_seq);
            
            /* Track consecutive timeouts */
            consecutive_timeouts++;
            DEBUG_PRINT("Consecutive timeouts: %d/%d\n", consecutive_timeouts, 15);
            
            /* Check if max timeouts reached */
            if (consecutive_timeouts >= 15) {
                DEBUG_PRINT("Maximum consecutive timeouts reached (%d)\n", consecutive_timeouts);
                DEBUG_PRINT("Assuming transfer complete (possibly with data loss)\n");
                DEBUG_PRINT("Transfer statistics:\n");
                DEBUG_PRINT("  Highest sequence received: %u\n", highest_received_seq);
                DEBUG_PRINT("  Next expected sequence: %u\n", expected_seq);
                DEBUG_PRINT("  Potential packets missing: %u\n", 
                        expected_seq > highest_received_seq+1 ? 
                        expected_seq - highest_received_seq - 1 : 0);
                
                /* Try one last time to get missing data */
                if (!eof_received) {
                    DEBUG_PRINT("Sending final SREJ request for seq=%u\n", highest_received_seq + 1);
                    send_srej_packet(sockfd, server_addr, highest_received_seq + 1);
                }
                finished = 1;
                break;
            }
        }
    }
    
    DEBUG_PRINT("Cleaning up sliding window\n");
    window_free(win);
}
