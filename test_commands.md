# Reliable UDP File Transfer Test Commands

This document provides the specific commands needed to run each of the required tests.

## Basic Test with Error Rate 0

**Purpose**: Verify that the 1-second select()/poll() in server child process never times out with error rate 0.

**Server Command**:
```bash
./server 0 5000 -d
```

**Client Command**:
```bash
./rcopy small.txt test_out_zero.txt 5 1000 0 localhost 5000 -d
```

**Expected Result**: No 1-second timeouts should be seen in the server child process.

## Basic Tests (with Bit Flips and Packet Drops)

### Test 1: Small File Transfer

**Purpose**: Test protocol with small file and moderate error rate.

**Server Command**:
```bash
./server 0.2 5000 -d
```

**Client Command**:
```bash
./rcopy small.txt test_out_small.txt 10 1000 0.2 localhost 5000 -d
```

**Metrics to Record**:
- Shortest run time across 3 attempts: ____________
- Server message counts (unique/total): ____________
- Client message counts (unique/total): ____________

### Test 2: Medium File Transfer

**Purpose**: Test protocol with medium file (~50KB) and moderate error rate.

**Server Command**:
```bash
./server 0.2 5000 -d
```

**Client Command**:
```bash
./rcopy medium.txt test_out_medium.txt 10 1000 0.2 localhost 5000 -d
```

**Metrics to Record**:
- Shortest run time across 3 attempts: ____________
- Server message counts (unique/total): ____________
- Client message counts (unique/total): ____________

### Test 3: Large File with Large Window

**Purpose**: Test protocol with large file (~420KB), large window, and low error rate.

**Server Command**:
```bash
./server 0.1 5000 -d
```

**Client Command**:
```bash
./rcopy big.txt test_out_big_large_window.txt 50 1000 0.1 localhost 5000 -d
```

**Metrics to Record**:
- Shortest run time across 3 attempts: ____________
- Server message counts (unique/total): ____________ (should be around 550 to pass)
- Client message counts (unique/total): ____________

### Test 4: Large File with Small Window

**Purpose**: Test protocol with large file (~420KB), small window, and moderate error rate.

**Server Command**:
```bash
./server 0.15 5000 -d
```

**Client Command**:
```bash
./rcopy big.txt test_out_big_small_window.txt 5 1000 0.15 localhost 5000 -d
```

**Metrics to Record**:
- Shortest run time across 3 attempts: ____________
- Server message counts (unique/total): ____________ (should be around 550 to pass)
- Client message counts (unique/total): ____________

## Special Cases

**Note**: Only perform these tests if all 4 basic tests pass.

### Test 5: Stop and Wait (Window Size 1)

**Purpose**: Test with stop-and-wait approach (window size = 1) and error rate.

**Server Command**:
```bash
./server 0.25 5000 -d
```

**Client Command**:
```bash
./rcopy medium.txt test_out_stop_wait.txt 1 1000 0.25 localhost 5000 -d
```

### Test 6: Server-Side Specific Packet Drops

**Purpose**: Test handling of specific packet drops on the server side.

**Server Command**:
```bash
./server 0.06 5000 -d
```
*Note: The server is set to drop packets 20-30 based on flag value 6*

**Client Command**:
```bash
./rcopy medium.txt test_out_server_drops.txt 10 1000 0.02 localhost 5000 -d
```
*Note: The client uses flag value 2*

### Test 7: Client-Side Specific Packet Drops

**Purpose**: Test handling of specific packet drops on the client side.

**Server Command**:
```bash
./server 0.02 5000 -d
```
*Note: The server uses flag value 2*

**Client Command**:
```bash
./rcopy medium.txt test_out_client_drops.txt 10 1000 0.07 localhost 5000 -d
```
*Note: The client is set to drop packets 15,18,30,31,35,37 based on flag value 7*

## Pre-Test Setup

Before running tests, generate the test files using:

```bash
python3 create.py
```

This will create:
- `small.txt` (~900 bytes)
- `medium.txt` (~50KB)
- `big.txt` (~420KB)

## Notes for Testing

1. Run each test 3 times if failures occur
2. Record the shortest successful run time
3. Count both unique and total messages for both client and server
4. Long run times (>30s) or high packet counts (>500) may indicate inefficiencies
5. Debug mode (-d) should be on for all tests to observe protocol behavior 