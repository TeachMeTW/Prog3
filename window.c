#include "window.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "debug.h"

// Create a new window structure to track packets
window_t *window_init(int window_size) {
    window_t *win = (window_t *)malloc(sizeof(window_t));
    if (!win) {
        perror("malloc");
        exit(1);
    }
    
    // Initialize window parameters
    win->window_size = window_size;  // Size comes from command line
    win->base = 0;  // Start with sequence 0
    win->next_seq = 0;  // Next packet will also be 0
    
    // Allocate memory for packet array - using calloc to zero everything out
    win->packets = (packet_t *)calloc(window_size, sizeof(packet_t));
    if (!win->packets) {
        perror("calloc");
        exit(1);
    }
    DEBUG_PRINT("WINDOW: Initialized with %d slots, base=%u\n", window_size, win->base);
    return win;
}

// Free all memory used by the window
void window_free(window_t *win) {
    if (!win)
        return;
        
    DEBUG_PRINT("WINDOW: Freeing window structure (base=%u)\n", win->base);
    int packet_count = 0;
    
    // Free all packet data buffers
    for (int i = 0; i < win->window_size; i++) {
        if (win->packets[i].data) {
            packet_count++;
            free(win->packets[i].data);
        }
    }
    
    DEBUG_PRINT("WINDOW: Freed %d buffered packets\n", packet_count);
    free(win->packets);
    free(win);
}

/* When adding a packet, we store its sequence number and clear its ack flag */
int window_add_packet(window_t *win, uint32_t seq_num, const char *data, int len, int flag) {
    int index = seq_num % win->window_size; // expected index based on sequence number
    
    // Sanity check - don't add packets too far ahead
    if (seq_num > win->base + 2 * win->window_size) {
        DEBUG_PRINT("Packet seq %u is far ahead. Current window range [%u, %u]\n",
                  seq_num, win->base, win->base + win->window_size - 1);
        return -2;
    }
    
    // If the slot is already taken with a different sequence number, try to find another slot
    if (win->packets[index].data != NULL && win->packets[index].seq_num != seq_num) {
        /* Debug output about what we're doing */
        if (!win->packets[index].acknowledged) {
            DEBUG_PRINT("Replacing existing packet seq %u at index %d\n", 
                      win->packets[index].seq_num, index);
        }
        
        // Only try to find alternate slot if sequence number is within window 
        if (seq_num >= win->base && seq_num < win->base + win->window_size) {
            // Try to find an empty or acknowledged slot 
            for (int i = 0; i < win->window_size; i++) {
                int alt_index = (index + i) % win->window_size;
                if (win->packets[alt_index].data == NULL || win->packets[alt_index].acknowledged) {
                    DEBUG_PRINT("Found alternate slot at index %d for packet seq %u\n",
                              alt_index, seq_num);
                    index = alt_index;
                    break;
                }
            }
        }
        
        // If the slot is still taken by a different sequence number,
        // forcibly replace it (buffer overruns are handled by the sliding window)
        if (win->packets[index].data != NULL && win->packets[index].seq_num != seq_num && !win->packets[index].acknowledged) {
            DEBUG_PRINT("WARNING: No available slots - forced to replace packet with seq %u with new seq %u at index %d\n",
                      win->packets[index].seq_num, seq_num, index);
        } else {
            DEBUG_PRINT("Replacing packet with seq %u with new seq %u at index %d\n",
                      win->packets[index].seq_num, seq_num, index);
        }
    }
    
    // If there's an existing packet with the same sequence number, free it
    if (win->packets[index].data != NULL && win->packets[index].seq_num == seq_num) {
        free(win->packets[index].data);
    } else {
        DEBUG_PRINT("Adding new packet seq %u at index %d\n", seq_num, index);
    }
    
    win->packets[index].seq_num = seq_num;
    win->packets[index].len = len;
    win->packets[index].flag = flag;
    win->packets[index].acknowledged = false;
    win->packets[index].retransmit_count = 0;
    win->packets[index].data = (char *)malloc(len);
    if (!win->packets[index].data) {
        perror("malloc");
        exit(1);
    }
    memcpy(win->packets[index].data, data, len);
    return index;
}

/* Improved window_mark_ack function to handle all cases correctly */
void window_mark_ack(window_t *win, uint32_t ack_seq) {
    // Special case: if ack sequence is just below window base, it could be a duplicated acknowledgment
    if (ack_seq == win->base - 1) {
        DEBUG_PRINT("WINDOW: Received ack for seq=%u (just below window base=%u)\n", 
                  ack_seq, win->base);
        
        // Track repeated ACKs for the same sequence number just below window 
        static uint32_t last_repeated_ack = 0;
        static int repeat_count = 0;
        
        if (last_repeated_ack == ack_seq) {
            repeat_count++;
            if (repeat_count >= 3) {
                DEBUG_PRINT("Detected repeated ack for seq %u (%d times)\n", 
                          ack_seq, repeat_count);
                
                // After receiving 3 duplicate ACKs, try to retransmit the following packet
                int missing_pkt_index = win->base % win->window_size;
                
                // Mark the packet at window base for retransmission
                if (win->packets[missing_pkt_index].data != NULL && 
                    win->packets[missing_pkt_index].seq_num == win->base) {
                    DEBUG_PRINT("Client appears to be missing packet %u - marking for retransmission\n", 
                              win->base);
                    win->packets[missing_pkt_index].acknowledged = false;
                }
                
                return;
            }
        } else {
            last_repeated_ack = ack_seq;
            repeat_count = 1;
        }
        
        return;
    }
    
    // Reset repeated ack tracking for non-edge cases
    if (ack_seq != win->base - 1) {
        static uint32_t last_repeated_ack = 0;
        static int repeat_count = 0;
        last_repeated_ack = 0;
        repeat_count = 0;
    }
    
    // If the ack is too old (and not just below the window), ignore it
    if (ack_seq < win->base && win->base - ack_seq > 5) {
        DEBUG_PRINT("WINDOW: Ignoring old ack for seq=%u (window base=%u, difference=%u)\n", 
               ack_seq, win->base, win->base - ack_seq);
        return;
    }

    // Calculate how many packets this ack covers
    uint32_t packets_to_ack = ack_seq >= win->base ? 
                             (ack_seq - win->base + 1) : 
                             (0xFFFFFFFF - win->base + ack_seq + 1);

    // Ensure we don't ack more than window_size packets at once
    if (packets_to_ack > win->window_size) {
        DEBUG_PRINT("WINDOW: Warning: Received ack for %u packets, limiting to window size %d\n", 
               packets_to_ack, win->window_size);
        packets_to_ack = win->window_size;
    }

    DEBUG_PRINT("WINDOW: Acknowledging packets from seq=%u to seq=%u (%u packets)\n", 
           win->base, ack_seq, packets_to_ack);

    // Mark packets as acknowledged
    for (uint32_t i = 0; i < packets_to_ack; i++) {
        uint32_t seq = win->base + i;
        int index = seq % win->window_size;
        
        // Only mark if this slot actually contains the correct packet
        if (win->packets[index].seq_num == seq) {
            win->packets[index].acknowledged = true;
            DEBUG_PRINT("Marked packet seq %u at index %d as acknowledged\n", seq, index);
        } else {
            DEBUG_PRINT("Packet at index %d has seq %u, expected %u\n", 
                   index, win->packets[index].seq_num, seq);
            
            // Search for the packet in other slots
            int found = 0;
            for (int j = 0; j < win->window_size; j++) {
                int alt_index = (index + j) % win->window_size;
                if (win->packets[alt_index].seq_num == seq) {
                    win->packets[alt_index].acknowledged = true;
                    DEBUG_PRINT("Marked packet seq %u at alternate index %d as acknowledged\n", 
                           seq, alt_index);
                    found = 1;
                    break;
                }
            }
            
            if (!found) {
                DEBUG_PRINT("Could not find packet seq %u in window to mark as acknowledged\n", seq);
            }
        }
    }
}

uint32_t window_get_base(window_t *win) {
    return win->base;
}

packet_t *window_get_packet(window_t *win, uint32_t seq_num) {
    // Expand the window range check to be more flexible
    // Allow access to any packet within a reasonable range from window base
    if (seq_num < win->base && win->base - seq_num > win->window_size) {
        DEBUG_PRINT("Requested packet seq %u is too old (window base: %u)\n", 
               seq_num, win->base);
        return NULL;
    }
    
    if (seq_num >= win->base + win->window_size * 2) {
        DEBUG_PRINT("Requested packet seq %u is too far ahead (window range [%u, %u])\n", 
               seq_num, win->base, win->base + win->window_size - 1);
        return NULL;
    }
    
    int index = seq_num % win->window_size;
    if (win->packets[index].seq_num == seq_num && win->packets[index].data != NULL)
        return &win->packets[index];
    
    // Search the entire window for the packet, not just the expected index
    for (int i = 0; i < win->window_size; i++) {
        int alt_index = (index + i) % win->window_size;
        if (win->packets[alt_index].seq_num == seq_num && win->packets[alt_index].data != NULL) {
            DEBUG_PRINT("Found packet seq %u at alternate index %d\n", seq_num, alt_index);
            return &win->packets[alt_index];
        }
    }
    
    // Additional debugging to understand why packet retrieval is failing
    if (win->packets[index].data == NULL) {
        DEBUG_PRINT("Packet at index %d has no data (NULL)\n", index);
    } else {
        DEBUG_PRINT("Packet at index %d has seq %u, not matching requested seq %u\n", 
               index, win->packets[index].seq_num, seq_num);
    }
    
    // If we reach here, the packet is not in the window
    DEBUG_PRINT("Packet seq %u not found in window\n", seq_num);
    return NULL;
}

/* Slide the window forward while the packet at the window base is acknowledged */
void window_slide(window_t *win) {
    uint32_t old_base = win->base;
    int packets_slid = 0;
    
    while (true) {
        int index = win->base % win->window_size;
        
        // Check if this packet slot contains the correct sequence number and is acknowledged
        if (win->packets[index].seq_num == win->base && win->packets[index].acknowledged) {
            // Free the data since we're done with this packet
            if (win->packets[index].data) {
                free(win->packets[index].data);
                win->packets[index].data = NULL;
            }
            
            // Clear the packet slot
            win->packets[index].seq_num = 0;
            win->packets[index].len = 0;
            win->packets[index].flag = 0;
            win->packets[index].acknowledged = false;
            win->packets[index].retransmit_count = 0;
            
            // Advance the window base
            win->base++;
            packets_slid++;
        } else {
            // If this slot doesn't contain the base packet, search the window
            int found = 0;
            for (int i = 0; i < win->window_size; i++) {
                int check_index = (index + i) % win->window_size;
                if (win->packets[check_index].seq_num == win->base && 
                    win->packets[check_index].acknowledged) {
                    // Found the packet elsewhere in window
                    if (win->packets[check_index].data) {
                        free(win->packets[check_index].data);
                        win->packets[check_index].data = NULL;
                    }
                    
                    // Clear the packet slot
                    win->packets[check_index].seq_num = 0;
                    win->packets[check_index].len = 0;
                    win->packets[check_index].flag = 0;
                    win->packets[check_index].acknowledged = false;
                    win->packets[check_index].retransmit_count = 0;
                    
                    win->base++;
                    packets_slid++;
                    found = 1;
                    break;
                }
            }
            
            if (!found) {
                break; // No more packets to slide
            }
        }
        
        // Safety check to prevent infinite loops - don't slide more than window_size at once
        if (packets_slid >= win->window_size) {
            break;
        }
    }
    
    if (packets_slid > 0) {
        DEBUG_PRINT("WINDOW: Slid from base=%u to base=%u (%d packets) [%.1f%% of window]\n", 
               old_base, win->base, packets_slid, 
               (float)packets_slid / win->window_size * 100.0);
    } else {
        DEBUG_PRINT("WINDOW: No sliding occurred (base remains at %u)\n", old_base);
    }
}
