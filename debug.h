/**
 * Debug utilities for reliable UDP file transfer
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>
#include <stdarg.h>

/* Global debug flag - set via command line -d flag */
extern int debug_enabled;

/* Initialize debug mode based on command arguments */
static inline void debug_init(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (argv[i] && argv[i][0] == '-' && argv[i][1] == 'd' && argv[i][2] == '\0') {
            debug_enabled = 1;
            break;
        }
    }
}

/* Debug print macro - only prints if debug_enabled is true */
#define DEBUG_PRINT(...) do { \
    if (debug_enabled) { \
        printf("[DEBUG] "); \
        printf(__VA_ARGS__); \
    } \
} while(0)

#endif /* DEBUG_H */ 