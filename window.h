#ifndef WINDOW_H
#define WINDOW_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>

/* This is where we store each packet in our sliding window
    a box that holds each packet we're waiting to get ACKed
 */
typedef struct {
    uint32_t seq_num;       // sequence number - unique ID for each packet
    int      len;           // total length of the PDU (header + payload)
    int      flag;          // packet type flag (e.g., FLAG_DATA)
    char    *data;          // pointer to complete PDU data
    bool     acknowledged;  // whether the packet has been ack’d
    int      retransmit_count; // count of retransmissions
} packet_t;

/* This is our sliding window implementation - it's basically a circular buffer 
   The "window" is the range of packets we're currently tracking
   It "slides" forward as packets get acknowledged */
typedef struct {
    packet_t *packets;  // array (allocated via malloc) of packet_t
    int       window_size;  // size in number of packets
    uint32_t  base;         // sequence number of oldest unacknowledged packet
    uint32_t  next_seq;     // next sequence number to use (sender only)
} window_t;

/* API functions for the window - call these instead of messing with the struct directly -- Cough cough the instructions */
// Create a new window with the specified size - don't forget to free it later!
window_t *window_init(int window_size);

// Free all memory associated with the window
void window_free(window_t *win);

// Add a packet to the window - returns -1 if window is full (wait for ACKs!)
int window_add_packet(window_t *win, uint32_t seq_num, const char *data, int len, int flag);

// Mark a packet as acknowledged - call this when you get an ACK
void window_mark_ack(window_t *win, uint32_t ack_seq);

// Get the base sequence number - useful to check if window has moved
uint32_t window_get_base(window_t *win);

// Find a packet in the window by its sequence number - returns NULL if not found
packet_t *window_get_packet(window_t *win, uint32_t seq_num);

// Slide the window forward, removing acknowledged packets - call this after marking ACKs
void window_slide(window_t *win);

#endif
