// Dart Bridge Daemon for Bitcoin Miner GUI
// Spawns miner.exe, parses stdout telemetry, and streams real-time data to Flutter Web over WebSocket.

import 'dart:async';
import 'dart:convert';
import 'dart:io';

Process? minerProcess;
final Set<WebSocket> clients = {};
bool isMining = false;

double liveMh = 0.0;
double cpuMh = 0.0;
double gpuMh = 0.0;
double avg5sMh = 0.0;
double totalAvgMh = 0.0;
int totalHashes = 0;
int acceptedShares = 0;
int rejectedShares = 0;
double currentDifficulty = 1.0;
String currentJobId = "-";
String currentPool = "solo.ckpool.org:3333";
int activeThreads = 6;
bool gpuEnabled = true;
String gpuDeviceName = "Intel(R) Iris(R) Xe Graphics (80 CUs)";
String miningMode = "stratum";
final List<String> logBuffer = [];

void broadcast(Map<String, dynamic> data) {
  final message = jsonEncode(data);
  for (final client in clients) {
    if (client.readyState == WebSocket.open) {
      client.add(message);
    }
  }
}

void appendLog(String line) {
  if (logBuffer.length > 500) logBuffer.removeAt(0);
  logBuffer.add(line);
  broadcast({"type": "log", "line": line});
}

void parseMinerLine(String line) {
  appendLog(line);

  // Parse GPU Engine Name
  if (line.contains("GPU Cryptographic Engine:")) {
    final gName = line.split("GPU Cryptographic Engine:").last.trim();
    if (!gName.contains("None detected")) {
      gpuDeviceName = gName;
      broadcast({"type": "gpu_info", "gpu_name": gpuDeviceName});
    }
  }

  // Parse Hybrid Live Telemetry:
  // [*] Live: 99.74 MH/s (CPU: 25.08 | GPU: 74.66 | 5s: 97.00 | Avg: 95.00) | Hashes: 503642626 | Accepted: 1 | Rejected: 0
  final hybridLiveRegex = RegExp(r'Live:\s*([\d\.]+)\s*MH/s\s*\(CPU:\s*([\d\.]+)\s*\|\s*GPU:\s*([\d\.]+)');
  final hMatch = hybridLiveRegex.firstMatch(line);
  if (hMatch != null) {
    liveMh = double.tryParse(hMatch.group(1) ?? "0") ?? liveMh;
    cpuMh = double.tryParse(hMatch.group(2) ?? "0") ?? cpuMh;
    gpuMh = double.tryParse(hMatch.group(3) ?? "0") ?? gpuMh;

    final avg5sMatch = RegExp(r'5s:\s*([\d\.]+)').firstMatch(line);
    if (avg5sMatch != null) avg5sMh = double.tryParse(avg5sMatch.group(1) ?? "0") ?? avg5sMh;

    final totalAvgMatch = RegExp(r'Avg:\s*([\d\.]+)').firstMatch(line);
    if (totalAvgMatch != null) totalAvgMh = double.tryParse(totalAvgMatch.group(1) ?? "0") ?? totalAvgMh;

    final hashMatch = RegExp(r'Hashes:\s*(\d+)').firstMatch(line);
    if (hashMatch != null) totalHashes = int.tryParse(hashMatch.group(1) ?? "0") ?? totalHashes;

    final accMatch = RegExp(r'Accepted:\s*(\d+)').firstMatch(line);
    if (accMatch != null) acceptedShares = int.tryParse(accMatch.group(1) ?? "0") ?? acceptedShares;

    final rejMatch = RegExp(r'Rejected:\s*(\d+)').firstMatch(line);
    if (rejMatch != null) rejectedShares = int.tryParse(rejMatch.group(1) ?? "0") ?? rejectedShares;

    broadcast({
      "type": "telemetry",
      "live_mh": liveMh,
      "cpu_mh": cpuMh,
      "gpu_mh": gpuMh,
      "avg_5s": avg5sMh,
      "total_avg": totalAvgMh,
      "total_hashes": totalHashes,
      "accepted": acceptedShares,
      "rejected": rejectedShares,
      "difficulty": currentDifficulty,
      "job_id": currentJobId,
      "is_mining": isMining,
      "gpu_name": gpuDeviceName,
      "mining_mode": miningMode,
    });
    return;
  }

  // Parse Legacy Live Telemetry:
  final liveRegex = RegExp(r'Live:\s*([\d\.]+)\s*MH/s\s*\(5s:\s*([\d\.]+)\s*\|\s*Avg:\s*([\d\.]+)\)\s*\|\s*Hashes:\s*(\d+)\s*\|\s*Accepted:\s*(\d+)\s*\|\s*Rejected:\s*(\d+)');
  final liveMatch = liveRegex.firstMatch(line);
  if (liveMatch != null) {
    liveMh = double.tryParse(liveMatch.group(1) ?? "0") ?? liveMh;
    avg5sMh = double.tryParse(liveMatch.group(2) ?? "0") ?? avg5sMh;
    totalAvgMh = double.tryParse(liveMatch.group(3) ?? "0") ?? totalAvgMh;
    totalHashes = int.tryParse(liveMatch.group(4) ?? "0") ?? totalHashes;
    acceptedShares = int.tryParse(liveMatch.group(5) ?? "0") ?? acceptedShares;
    rejectedShares = int.tryParse(liveMatch.group(6) ?? "0") ?? rejectedShares;

    broadcast({
      "type": "telemetry",
      "live_mh": liveMh,
      "cpu_mh": cpuMh,
      "gpu_mh": gpuMh,
      "avg_5s": avg5sMh,
      "total_avg": totalAvgMh,
      "total_hashes": totalHashes,
      "accepted": acceptedShares,
      "rejected": rejectedShares,
      "difficulty": currentDifficulty,
      "job_id": currentJobId,
      "is_mining": isMining,
      "gpu_name": gpuDeviceName,
      "mining_mode": miningMode,
    });
    return;
  }

  // Parse: [+] Pool Difficulty Updated: 65536.00
  final diffRegex = RegExp(r'Pool Difficulty Updated:\s*([\d\.]+)');
  final diffMatch = diffRegex.firstMatch(line);
  if (diffMatch != null) {
    currentDifficulty = double.tryParse(diffMatch.group(1) ?? "1.0") ?? currentDifficulty;
    broadcast({"type": "difficulty", "difficulty": currentDifficulty});
    return;
  }

  // Parse: [+] New Mining Job Received: JobID=...
  final jobRegex = RegExp(r'JobID=([a-fA-F0-9]+)');
  final jobMatch = jobRegex.firstMatch(line);
  if (jobMatch != null) {
    currentJobId = jobMatch.group(1) ?? currentJobId;
    broadcast({"type": "job", "job_id": currentJobId});
    return;
  }

  // Parse: SHARE ACCEPTED!
  if (line.contains("SHARE ACCEPTED") || line.contains("BLOCK ACCEPTED")) {
    acceptedShares++;
    broadcast({"type": "accepted", "accepted": acceptedShares});
  }
}

Future<void> startMining(Map<String, dynamic> params) async {
  if (isMining) return;

  final mode = params["mode"] as String? ?? "stratum";
  miningMode = mode;
  final enableGpu = params["enable_gpu"] as bool? ?? true;
  gpuEnabled = enableGpu;

  final pool = params["pool"] as String? ?? "solo.ckpool.org";
  final port = (params["port"] is num) ? (params["port"] as num).toInt() : int.tryParse(params["port"]?.toString() ?? "3333") ?? 3333;
  final user = params["user"] as String? ?? "1EUeWGhrKsrSmicJoSgYVgbbNAT4hFBzqS";
  final password = params["password"] as String? ?? "x";
  final threads = (params["threads"] is num) ? (params["threads"] as num).toInt() : int.tryParse(params["threads"]?.toString() ?? "6") ?? 6;

  final rpcUrl = params["rpc_url"] as String? ?? "http://127.0.0.1:8332";
  final rpcUser = params["rpc_user"] as String? ?? "bitcoin";
  final rpcPassword = params["rpc_password"] as String? ?? "password";

  currentPool = mode == "btcrpc" ? rpcUrl : "$pool:$port";
  activeThreads = threads;
  liveMh = 0.0;
  cpuMh = 0.0;
  gpuMh = 0.0;
  avg5sMh = 0.0;
  totalAvgMh = 0.0;
  totalHashes = 0;
  acceptedShares = 0;
  rejectedShares = 0;

  final args = <String>[];
  if (mode == "btcrpc") {
    appendLog("[*] Launching miner.exe in Bitcoin Core RPC Solo Mode ($rpcUrl | $user)...");
    args.addAll([
      "--rpc-url", rpcUrl,
      "--rpc-user", rpcUser,
      "--rpc-password", rpcPassword,
      "--user", user,
      "--threads", threads.toString(),
    ]);
  } else {
    appendLog("[*] Launching miner.exe ($pool:$port | $user | $threads threads)...");
    args.addAll([
      "--pool", pool,
      "--port", port.toString(),
      "--user", user,
      "--password", password,
      "--threads", threads.toString(),
    ]);
  }

  if (!enableGpu) {
    args.add("--no-gpu");
  } else {
    args.add("--gpu");
  }

  try {
    minerProcess = await Process.start(
      "miner.exe",
      args,
      workingDirectory: Directory.current.path,
    );

    isMining = true;
    broadcast({
      "type": "status",
      "is_mining": true,
      "pool": currentPool,
      "threads": activeThreads,
      "gpu_enabled": gpuEnabled,
      "mining_mode": miningMode,
    });

    minerProcess!.stdout.transform(utf8.decoder).transform(const LineSplitter()).listen(parseMinerLine);
    minerProcess!.stderr.transform(utf8.decoder).transform(const LineSplitter()).listen((err) => appendLog("[-] $err"));

    minerProcess!.exitCode.then((code) {
      isMining = false;
      minerProcess = null;
      appendLog("[*] miner.exe exited with code $code");
      broadcast({"type": "status", "is_mining": false});
    });
  } catch (e) {
    appendLog("[-] Failed to launch miner.exe: $e");
    isMining = false;
    broadcast({"type": "status", "is_mining": false});
  }
}

void stopMining() {
  if (!isMining || minerProcess == null) return;
  appendLog("[*] Stopping miner.exe...");
  minerProcess?.kill(ProcessSignal.sigterm);
  Process.run("taskkill", ["/F", "/IM", "miner.exe"]);
  isMining = false;
  broadcast({"type": "status", "is_mining": false});
}

Timer? autoShutdownTimer;

void checkAutoShutdown() {
  if (clients.isEmpty) {
    autoShutdownTimer?.cancel();
    autoShutdownTimer = Timer(const Duration(seconds: 25), () {
      if (clients.isEmpty) {
        print("[*] All UI windows closed. Cleaning up and shutting down bridge...");
        stopMining();
        exit(0);
      }
    });
  } else {
    autoShutdownTimer?.cancel();
    autoShutdownTimer = null;
  }
}

Future<void> main() async {
  final staticDir = Directory("gui/build/web");
  final server = await HttpServer.bind(InternetAddress.loopbackIPv4, 8080);
  print("[+] Bitcoin Miner GUI Bridge running at http://localhost:8080");

  // Initial grace period for window to open
  checkAutoShutdown();

  server.listen((HttpRequest request) async {
    if (request.uri.path == "/ws") {
      final socket = await WebSocketTransformer.upgrade(request);
      clients.add(socket);
      checkAutoShutdown();

      // Send initial state
      socket.add(jsonEncode({
        "type": "init",
        "is_mining": isMining,
        "live_mh": liveMh,
        "cpu_mh": cpuMh,
        "gpu_mh": gpuMh,
        "avg_5s": avg5sMh,
        "total_avg": totalAvgMh,
        "total_hashes": totalHashes,
        "accepted": acceptedShares,
        "rejected": rejectedShares,
        "difficulty": currentDifficulty,
        "job_id": currentJobId,
        "pool": currentPool,
        "threads": activeThreads,
        "gpu_enabled": gpuEnabled,
        "gpu_name": gpuDeviceName,
        "mining_mode": miningMode,
        "logs": logBuffer,
      }));

      socket.listen(
        (data) {
          try {
            final msg = jsonDecode(data as String) as Map<String, dynamic>;
            final action = msg["action"] as String?;
            if (action == "start") {
              startMining(msg);
            } else if (action == "stop") {
              stopMining();
            }
          } catch (e) {
            print("[-] Error processing client message: $e");
          }
        },
        onDone: () {
          clients.remove(socket);
          checkAutoShutdown();
        },
        onError: (e) {
          clients.remove(socket);
          checkAutoShutdown();
        },
      );
      return;
    }

    // Serve static Flutter Web files
    String filePath = request.uri.path == "/" ? "/index.html" : request.uri.path;
    final file = File("${staticDir.path}$filePath");

    if (await file.exists()) {
      final mimeType = filePath.endsWith(".html")
          ? "text/html; charset=utf-8"
          : (filePath.endsWith(".js") || filePath.endsWith(".mjs"))
              ? "application/javascript; charset=utf-8"
              : filePath.endsWith(".json")
                  ? "application/json; charset=utf-8"
                  : filePath.endsWith(".wasm")
                      ? "application/wasm"
                      : filePath.endsWith(".png")
                          ? "image/png"
                          : filePath.endsWith(".css")
                              ? "text/css; charset=utf-8"
                              : filePath.endsWith(".ttf")
                                  ? "font/ttf"
                                  : filePath.endsWith(".otf")
                                      ? "font/otf"
                                      : filePath.endsWith(".woff2")
                                          ? "font/woff2"
                                          : "application/octet-stream";
      request.response.headers.contentType = ContentType.parse(mimeType);
      await file.openRead().pipe(request.response);
    } else {
      final indexFile = File("${staticDir.path}/index.html");
      if (await indexFile.exists()) {
        request.response.headers.contentType = ContentType.html;
        await indexFile.openRead().pipe(request.response);
      } else {
        request.response
          ..statusCode = HttpStatus.notFound
          ..write("Flutter Web build not found. Please build the GUI first.");
        await request.response.close();
      }
    }
  });

  ProcessSignal.sigint.watch().listen((_) {
    stopMining();
    exit(0);
  });
}
