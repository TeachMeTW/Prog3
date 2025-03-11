#include <string.h>
#include <stdio.h>
#include "circular_buffer.h"

/* Initialize a circular buffer */
circular_buffer_t *circular_buffer_init(size_t size, uint32_t buffer_size) {
    // First allocate the struct itself
    circular_buffer_t *cb = (circular_buffer_t *)malloc(sizeof(circular_buffer_t));
    if (!cb) {
        perror("malloc");
        exit(1);
    }
    
    // Now allocate the actual buffer space
    cb->data = (char *)malloc(size);  // This is where all our packet data will be at
    if (!cb->data) {
        perror("malloc");
        free(cb);
        exit(1);
    }
    
    // Initialize all the tracking variables to their starting values
    cb->size = size;  // Total size of our buffer
    cb->head = 0;     // Start reading from the beginning
    cb->tail = 0;     // Start writing at the beginning
    cb->bytes_stored = 0;  // Buffer starts empty
    cb->start_seq = 0;  // First sequence number (will be updated when we add packets)
    cb->end_seq = 0;    // Last sequence number (also updated when adding packets)
    cb->buffer_size = buffer_size;  // Size of each individual packet's buffer
    
    return cb;
}

/* Free the circular buffer */
void circular_buffer_free(circular_buffer_t *cb) {
    if (cb) {
        if (cb->data) {
            free(cb->data);
        }
        free(cb);
    }
}

/* Write data to the circular buffer */
int circular_buffer_write(circular_buffer_t *cb, const char *data, size_t len, uint32_t seq_num) {
    if (cb->bytes_stored + len > cb->size) {
        // Buffer is full, slide window by removing oldest entries
        // Calculate how many oldest packets to remove
        size_t bytes_to_free = len;
        uint32_t seqs_to_remove = 0;
        
        while (bytes_to_free > 0 && cb->bytes_stored > 0) {
            size_t packet_size = (seqs_to_remove == 0) ? 
                                 cb->buffer_size - (cb->head % cb->buffer_size) : 
                                 cb->buffer_size;
            
            if (packet_size > cb->bytes_stored) {
                packet_size = cb->bytes_stored;
            }
            
            cb->head = (cb->head + packet_size) % cb->size;
            cb->bytes_stored -= packet_size;
            bytes_to_free -= (bytes_to_free > packet_size) ? packet_size : bytes_to_free;
            seqs_to_remove++;
        }
        
        cb->start_seq += seqs_to_remove;
        
        // If still not enough space, return error
        if (cb->bytes_stored + len > cb->size) {
            return -1;
        }
    }
    
    // Write data to buffer
    if (cb->tail + len <= cb->size) {
        // Simple case - no wrap around
        memcpy(cb->data + cb->tail, data, len);
    } else {
        // Data wraps around buffer end
        size_t first_chunk = cb->size - cb->tail;
        memcpy(cb->data + cb->tail, data, first_chunk);
        memcpy(cb->data, data + first_chunk, len - first_chunk);
    }
    
    cb->tail = (cb->tail + len) % cb->size;
    cb->bytes_stored += len;
    
    // Update end sequence number if this is a new sequence number
    if (seq_num >= cb->end_seq) {
        cb->end_seq = seq_num + 1;
    }
    
    return 0;
}

/* Read data from the circular buffer for a specific sequence number */
int circular_buffer_read_seq(circular_buffer_t *cb, char *buffer, size_t max_len, uint32_t seq_num) {
    // Check if the requested sequence is within our buffer
    if (seq_num < cb->start_seq || seq_num >= cb->end_seq) {
        return -1; // Sequence not in buffer
    }
    
    // Calculate position in buffer for this sequence
    uint32_t seq_offset = seq_num - cb->start_seq;
    size_t position = (cb->head + (seq_offset * cb->buffer_size)) % cb->size;
    
    // Read up to max_len or the packet size
    size_t len = cb->buffer_size;
    if (len > max_len) {
        len = max_len;
    }
    
    // Check if we have enough data
    if (seq_num == cb->end_seq - 1 && cb->bytes_stored < (seq_offset + 1) * cb->buffer_size) {
        // Last packet might be smaller
        size_t bytes_in_last_packet = cb->bytes_stored - (seq_offset * cb->buffer_size);
        if (bytes_in_last_packet < len) {
            len = bytes_in_last_packet;
        }
    }
    
    // Copy data from circular buffer to output buffer
    if (position + len <= cb->size) {
        // Simple case - no wrap around
        memcpy(buffer, cb->data + position, len);
    } else {
        // Data wraps around buffer end
        size_t first_chunk = cb->size - position;
        memcpy(buffer, cb->data + position, first_chunk);
        memcpy(buffer + first_chunk, cb->data, len - first_chunk);
    }
    
    return len;
} 