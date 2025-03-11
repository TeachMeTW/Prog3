#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>

/* Constants for packet sizes */
#define MAX_DATA_SIZE   1400    // Maximum size for data payload (stays below typical MTU)
#define HEADER_SIZE     7       // 7 bytes for our header (4+2+1)
#define MAX_PDU_SIZE    (HEADER_SIZE + MAX_DATA_SIZE)  // Total packet size

/* Protocol Constants - kinda arbritrary really */
#define INIT_BUF_SIZE 1024     // Size of buffer for initial packet - needs to be big enough for filename
#define MAX_RETRANSMIT 10      // Maximum number of retransmissions before giving up - too many and it takes forever
#define INIT_RETRY_LIMIT 10    // Maximum retries for initialization - lots of retries because this is critical
#define DATA_TIMEOUT 10000     // 1 seconds timeout for data packets
#define FINAL_TIMEOUT 3000     // 3 seconds timeout for final EOF ack 

/* Flag definitions */
#define FLAG_RR               5    // Receiver Ready (ack)
#define FLAG_SREJ             6    // Selective Reject 
#define FLAG_FILENAME         8    // Filename request (from rcopy to server)
#define FLAG_FILENAME_RESP    9    // Response to filename request
#define FLAG_EOF             10    // End-of-file (last data packet)
#define FLAG_DATA            16    // Regular data packet
#define FLAG_RESENT_SREJ     17    // Resent data packet due to SREJ 
#define FLAG_RESENT_TIMEOUT  18    // Resent data packet due to timeout 

/* This struct defines our PDU header format
   #pragma pack makes sure there's no padding between fields
   so the struct is exactly 7 bytes  */
#pragma pack(push, 1)
typedef struct {
    uint32_t seq_num;    // 32-bit sequence number (network byte order)
    uint16_t checksum;   // Internet checksum
    uint8_t  flag;       // Type flag

} pdu_header_t;
#pragma pack(pop)

#pragma pack(push, 1)
typedef struct {
    char     filename[101];  // max 100 chars + null - the file we want to transfer
    uint32_t window_size;    // in packets (network byte order) - how many packets we can have in flight
    uint32_t buffer_size;    // in bytes (network byte order) - size of each packet's data
} init_payload_t;
#pragma pack(pop)

#endif
