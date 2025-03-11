PLEASE READ THE WHOLE THING PLEASE

Name: Robin Simpson
Lab Section: 3PM

Demo Vid: https://www.youtube.com/watch?v=mWQk61cgoK4

## Overview

- Sliding window for flow control
- Selective repeat for efficient retransmission
- Checksums for error detection
- Timeouts for lost packet recovery
- Sequence numbers for packet ordering
- Maybe repeated and/or inefficient code because I've been up for 16 hours/day till 4 am to develop this (not sponsored by redbull)

## Architecture

The system consists of two main components:

1. **Server (`server`)**: Listens for client requests and serves requested files
2. **Client (`rcopy`)**: Requests files from the server

The implementation uses a client-server architecture where:
- The server handles multiple clients simultaneously using `fork()`
- Each file transfer uses an independent sliding window mechanism
- The protocol handles packet loss, corruption, and reordering
- Debug output can be enabled to see the protocol in action (PLEASE USE WHEN UR GRADING/TESTING!)

### Key Components

- **Protocol Design**: Custom PDU (Protocol Data Unit) with headers and checksums
- **Window Management**: Sliding window implementation with selective repeat
- **Circular Buffer**: Efficient storage for packets within the window
- **Buggy and Archaic Code**: Nobody said it was going to be the best

## Protocol Specification

### PDU Format

Each packet consists of a 7-byte header followed by an optional data payload:

```
+----------------+----------------+----------------+
| Sequence Number| Checksum       | Flag           |
| (4 bytes)      | (2 bytes)      | (1 byte)       |
+----------------+----------------+----------------+
|                Data Payload                      |
|            (0-1400 bytes)                        |
+----------------+----------------+----------------+
```

### Protocol Flags

The protocol uses the following flag values:

| Flag Value | Name              | Description                                    |
|------------|-------------------|------------------------------------------------|
| 5          | RR                | Receiver Ready (acknowledgment)                |
| 6          | SREJ              | Selective Reject (request for retransmission)  |
| 8          | FILENAME          | Filename request from client                   |
| 9          | FILENAME_RESP     | Server response to filename request            |
| 10         | EOF               | End-of-file (last data packet)                 |
| 16         | DATA              | Regular data packet                            |
| 17         | RESENT_SREJ       | Resent data packet after SREJ                  |
| 18         | RESENT_TIMEOUT    | Resent data packet after timeout               |
| 69420      | GIVE_UP           | Me after finding 50k bugs                      |

### Protocol Operation

1. **Connection Establishment**:
   - Client sends FILENAME request with requested file and negotiated parameters
   - Server responds with FILENAME_RESP ("OK" if file exists)

2. **Data Transfer**:
   - Server sends DATA packets within the sliding window
   - Client acknowledges with RR packets or requests retransmission with SREJ
   - Server adjusts window based on acknowledgments

3. **Connection Termination**:
   - Server sends EOF packet when file transfer is complete
   - Client acknowledges EOF packet multiple times for reliability

## Compilation

Compile the project by running:

```
make
```

This will build both the server and client executables.

## Usage

### Server

Start the server with:

```
./server error-rate [optional-port-number] [-d]
```

Where:
- `error-rate` is a floating point number between 0 and 1 (e.g., 0.1 for 10% errors)
- `optional-port-number` is optional; if not provided, the system will assign a port
- `-d` is debug mode (optional), shows detailed protocol operation

Example:
```
./server 0.1 5000 -d
```
This starts the server with 10% error rate on port 5000 with debug output enabled.

### Client (rcopy)

Request a file with:

```
./rcopy from-filename to-filename window-size buffer-size error-rate remote-machine remote-port [-d]
```

Where:
- `from-filename` is the name of the file to download from the server
- `to-filename` is the name of the file to save locally
- `window-size` is the size of the sliding window in packets
- `buffer-size` is the number of bytes per data packet (max 1400)
- `error-rate` is a floating point number between 0 and 1
- `remote-machine` is the hostname of the server
- `remote-port` is the port number the server is using
- `-d` is debug mode (optional), shows detailed protocol operation

Example:
```
./rcopy test.pdf local-test.pdf 16 1400 0.1 localhost 5000 -d
```
This requests `test.pdf` from the server running on localhost:5000, saving it as `local-test.pdf`, using a window size of 16 packets, 1400 bytes per packet, with 10% simulated errors, and debug output enabled.

## Implementation Details

### Sliding Window

The implementation uses a sliding window mechanism that:
- Buffers out-of-order packets
- Tracks acknowledgments
- Handles window advancement
- Manages retransmissions

The window size is configurable, allowing adjustment for different network conditions.

### Error Handling

Several error scenarios are handled:

1. **Packet Loss**: Detected via timeout, triggers retransmission
2. **Packet Corruption**: Detected via checksum, triggers selective retransmission
3. **Packet Reordering**: Handled by buffering out-of-order packets
4. **Duplicate Packets**: Ignored based on sequence numbers

### Timeouts

The implementation uses several timeout values:
- `DATA_TIMEOUT`: Timeout for data packets
- `FINAL_TIMEOUT`  Timeout for final EOF acknowledgment

### Safe Network Functions

The implementation includes safe wrappers for network functions that check for errors:
- `safeSendto()`: Safe wrapper for `sendto()`
- `safeRecvfrom()`: Safe wrapper for `recvfrom()`

## Testing

The included `create.py` script can generate test files of various sizes:
- `small.txt`: ~900 bytes
- `medium.txt`: ~50 KB
- `big.txt`: ~420 KB

Run the script with:
```
python3 create.py
```

## Implementation Notes

This implementation:
- Uses `sendtoErr()` for all packet transmissions -- so yes I did use it!
- Uses `poll()` for flow control
- Implements circular buffer for sliding window
- Handles multiple clients simultaneously via `fork()`
- Supports both IPv4 and IPv6 addressing 


if smt doesnt work maybe makefile edit to:
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@ to include the $(LIBS) 