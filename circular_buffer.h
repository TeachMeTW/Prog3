#ifndef CIRCULAR_BUFFER_H
#define CIRCULAR_BUFFER_H

#include <stdint.h>
#include <stdlib.h>

/* The circular buffer structure */
typedef struct {
    char *data;           // Buffer data
    size_t size;          // Total buffer size in bytes
    size_t head;          // Current read position
    size_t tail;          // Current write position
    size_t bytes_stored;  // Number of bytes currently stored
    uint32_t start_seq;   // First sequence number in buffer
    uint32_t end_seq;     // Last sequence number in buffer
    uint32_t buffer_size; // Size of each packet's buffer
} circular_buffer_t;

/* Initialize a circular buffer - call this first
   Returns a pointer to a new circular buffer structure */
circular_buffer_t *circular_buffer_init(size_t size, uint32_t buffer_size);

/* Free the circular buffer */
void circular_buffer_free(circular_buffer_t *cb);

/* Write data to the circular buffer 
   Returns the number of bytes written, or -1 if buffer is full
   This is kinda tricky because the buffer might wrap around */
int circular_buffer_write(circular_buffer_t *cb, const char *data, size_t len, uint32_t seq_num);

/* Read data from the circular buffer for a specific sequence number */
int circular_buffer_read_seq(circular_buffer_t *cb, char *buffer, size_t max_len, uint32_t seq_num);

#endif /* CIRCULAR_BUFFER_H */ 