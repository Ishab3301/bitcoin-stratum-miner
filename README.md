# Bitcoin Stratum CPU Miner (Intel/AMD SHA-NI Accelerated)

An educational, high-performance open-source Bitcoin CPU miner written in pure C featuring Intel/AMD SHA-NI hardware acceleration, a dedicated Flutter GUI dashboard, and full Stratum V1 protocol support.

---

## Performance Benchmark

By leveraging direct hardware-accelerated SHA extensions (`__SHA__`, Intel SHA-NI / AMD Zen SHA instructions), this miner jumps from standard scalar speeds of ~4.5 MH/s to **27–30 MH/s** on modern CPUs — achieving a **~6.6x speedup** with zero external cryptographic dependencies.

| Engine | Single Core | 6 Threads | 8 Threads |
| :--- | :--- | :--- | :--- |
| **Standard Scalar (Software)** | ~0.75 MH/s | ~4.50 MH/s | ~5.80 MH/s |
| **Intel SHA-NI (Hardware Accelerated)** | **~3.85 MH/s** | **~27.1 MH/s** | **~29.7+ MH/s** |

---

## Features

- **Intel & AMD SHA-NI Acceleration**: Utilizes hardware `_mm_sha256rnds2_epu32`, `_mm_sha256msg1_epu32`, and `_mm_sha256msg2_epu32` instructions for lightning-fast block double-hashing.
- **Dedicated Desktop GUI (Flutter)**: Modern dark dashboard with real-time hashrate graph, live telemetry cards (accepted, rejected, diff), pool presets, and embedded terminal console.
- **Silent Launcher**: Launches directly as a native standalone application window with zero black command prompt clutter.
- **9-Stage Verification Suite**: Automated cryptographic self-test verifying NIST test vectors, Genesis Block midstate double-SHA256, multi-nonce equivalence, and Stratum JSON-RPC parser fixtures on startup.
- **Stratum V1 Client**: Background receiver thread processing `mining.notify`, `mining.set_difficulty`, and out-of-order `mining.submit` share tracking.
- **Pure C with Zero External Crypto Dependencies**: Completely self-contained with embedded JSMN parser and Windows Socket 2 / POSIX compatibility.

---

## Quick Start (Dedicated GUI)

### Option 1: Desktop Shortcut (Recommended)
Double-click the **Bitcoin Miner** shortcut on your Desktop. It launches the miner GUI in a clean standalone window with **zero command prompt clutter**.

### Option 2: Silent VBScript
Double-click `Start_Miner_GUI.vbs` in this folder.

### Option 3: Batch Script
Double-click `run_gui.bat` or run from terminal:
```cmd
run_gui.bat
```

---

## Compilation Instructions

### MinGW-w64 / GCC (with Hardware SHA-NI):
```cmd
gcc miner.c -O3 -march=native -o miner.exe -lws2_32
```
> **Note**: `-march=native` automatically detects and activates Intel/AMD SHA-NI hardware instructions.

### Visual Studio (MSVC):
```cmd
cl /O2 miner.c ws2_32.lib
```

---

## Usage & Command Line Options

### Run Built-in Benchmark (e.g. 5 seconds, 8 threads):
```cmd
miner.exe --benchmark 5 --threads 8
```

### Run Cryptographic Self-Tests Only:
```cmd
miner.exe --test
```

### Connect to Solo Mining Pool (default: solo.ckpool.org:3333):
```cmd
miner.exe --pool solo.ckpool.org --port 3333 --user 1EUeWGhrKsrSmicJoSgYVgbbNAT4hFBzqS --threads 6
```

### Display Help:
```cmd
miner.exe --help
```

---

## Options Summary
- `--pool <host>`: Mining pool hostname (default: `solo.ckpool.org`)
- `--port <port>`: Stratum TCP port (default: `3333`)
- `--user <address>`: Pool username / BTC payout address
- `--password <pass>`: Pool password (default: `x`)
- `--threads <num>`: Number of CPU threads (default: `6`)
- `--benchmark [sec]`: Run local speed benchmark for `sec` seconds (default: `5`)
- `--test`: Run cryptographic self-test and exit
- `--help`: Show usage guide

---

## Educational Disclaimer

Bitcoin mining difficulty on mainnet requires dedicated ASIC hardware (TeraHashes/sec). This project is built for educational exploration of Bitcoin's Proof-of-Work internals, SHA-256 SIMD hardware intrinsics, and the Stratum network protocol.
