import 'dart:async';
import 'dart:convert';
import 'dart:math' as math;
import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:web_socket_channel/web_socket_channel.dart';
import 'dart:js_interop';

@JS('playShareAcceptedChime')
external void _playShareAcceptedChime();

void main() {
  runApp(const BtcMinerApp());
}

class BtcMinerApp extends StatelessWidget {
  const BtcMinerApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Bitcoin Stratum & Solo RPC Miner',
      debugShowCheckedModeBanner: false,
      themeMode: ThemeMode.dark,
      theme: ThemeData(
        brightness: Brightness.dark,
        scaffoldBackgroundColor: const Color(0xFF0D1117),
        cardColor: const Color(0xFF161B22),
        primaryColor: const Color(0xFFF7931A),
        colorScheme: const ColorScheme.dark(
          primary: Color(0xFFF7931A),
          secondary: Color(0xFF58A6FF),
          surface: Color(0xFF161B22),
          error: Color(0xFFF85149),
        ),
        fontFamily: 'Segoe UI',
        useMaterial3: true,
      ),
      home: const MinerDashboard(),
    );
  }
}

enum PoolPreset { soloCkPool, binancePool, custom }

class MinerDashboard extends StatefulWidget {
  const MinerDashboard({super.key});

  @override
  State<MinerDashboard> createState() => _MinerDashboardState();
}

class _MinerDashboardState extends State<MinerDashboard> with SingleTickerProviderStateMixin {
  WebSocketChannel? _channel;
  Timer? _reconnectTimer;
  bool _isConnected = false;
  bool _isMining = false;

  // Telemetry
  double _liveMh = 0.0;
  double _cpuMh = 0.0;
  double _gpuMh = 0.0;
  double _avg5sMh = 0.0;
  double _totalAvgMh = 0.0;
  int _totalHashes = 0;
  int _acceptedShares = 0;
  int _rejectedShares = 0;
  double _difficulty = 1.0;
  String _jobId = "-";
  int _threads = 8;
  bool _gpuEnabled = true;
  String _gpuName = "Intel(R) Iris(R) Xe Graphics";
  bool _isRpcMode = false; // false = Stratum V1 Pool, true = Bitcoin Core RPC Solo

  final List<double> _hashrateHistory = List.filled(40, 0.0);
  final List<String> _logs = [];
  final ScrollController _logScrollController = ScrollController();
  bool _autoScroll = true;

  // Stratum Presets and Controllers
  PoolPreset _selectedPreset = PoolPreset.soloCkPool;
  final TextEditingController _poolController = TextEditingController(text: "solo.ckpool.org");
  final TextEditingController _portController = TextEditingController(text: "3333");
  final TextEditingController _userController = TextEditingController(text: "1EUeWGhrKsrSmicJoSgYVgbbNAT4hFBzqS");
  final TextEditingController _passController = TextEditingController(text: "x");

  // Bitcoin Core RPC Solo Controllers
  final TextEditingController _rpcUrlController = TextEditingController(text: "http://127.0.0.1:8332");
  final TextEditingController _rpcUserController = TextEditingController(text: "bitcoin");
  final TextEditingController _rpcPassController = TextEditingController(text: "password");
  final TextEditingController _rpcAddressController = TextEditingController(text: "1EUeWGhrKsrSmicJoSgYVgbbNAT4hFBzqS");

  late AnimationController _pulseController;
  late Animation<double> _pulseAnimation;

  @override
  void initState() {
    super.initState();
    _pulseController = AnimationController(
      vsync: this,
      duration: const Duration(milliseconds: 1400),
    )..repeat(reverse: true);
    _pulseAnimation = Tween<double>(begin: 0.85, end: 1.0).animate(
      CurvedAnimation(parent: _pulseController, curve: Curves.easeInOut),
    );

    _connectWebSocket();
  }

  @override
  void dispose() {
    _reconnectTimer?.cancel();
    _channel?.sink.close();
    _pulseController.dispose();
    _logScrollController.dispose();
    _poolController.dispose();
    _portController.dispose();
    _userController.dispose();
    _passController.dispose();
    _rpcUrlController.dispose();
    _rpcUserController.dispose();
    _rpcPassController.dispose();
    _rpcAddressController.dispose();
    super.dispose();
  }

  void _triggerShareChime() {
    if (kIsWeb) {
      try {
        _playShareAcceptedChime();
      } catch (_) {}
    }
  }

  void _applyPreset(PoolPreset preset) {
    setState(() {
      _selectedPreset = preset;
      if (preset == PoolPreset.soloCkPool) {
        _poolController.text = "solo.ckpool.org";
        _portController.text = "3333";
        _userController.text = "1EUeWGhrKsrSmicJoSgYVgbbNAT4hFBzqS";
        _passController.text = "x";
      } else if (preset == PoolPreset.binancePool) {
        _poolController.text = "sha256.poolbinance.com";
        _portController.text = "443";
        _userController.text = "330133013301.001";
        _passController.text = "123456";
      }
    });
  }

  void _connectWebSocket() {
    _reconnectTimer?.cancel();
    try {
      final host = Uri.base.host.isEmpty ? "localhost" : Uri.base.host;
      final port = Uri.base.port == 0 ? 8080 : Uri.base.port;
      final wsUrl = Uri.parse("ws://$host:$port/ws");

      _channel = WebSocketChannel.connect(wsUrl);
      _channel!.stream.listen(
        _handleServerMessage,
        onDone: () {
          setState(() => _isConnected = false);
          _scheduleReconnect();
        },
        onError: (err) {
          setState(() => _isConnected = false);
          _scheduleReconnect();
        },
      );
      setState(() => _isConnected = true);
    } catch (_) {
      setState(() => _isConnected = false);
      _scheduleReconnect();
    }
  }

  void _scheduleReconnect() {
    _reconnectTimer?.cancel();
    _reconnectTimer = Timer(const Duration(seconds: 3), () {
      if (!_isConnected) _connectWebSocket();
    });
  }

  void _handleServerMessage(dynamic message) {
    try {
      final data = jsonDecode(message as String) as Map<String, dynamic>;
      final type = data["type"] as String?;

      setState(() {
        _isConnected = true;

        if (type == "init") {
          _isMining = data["is_mining"] as bool? ?? false;
          _liveMh = (data["live_mh"] as num?)?.toDouble() ?? 0.0;
          _cpuMh = (data["cpu_mh"] as num?)?.toDouble() ?? 0.0;
          _gpuMh = (data["gpu_mh"] as num?)?.toDouble() ?? 0.0;
          _avg5sMh = (data["avg_5s"] as num?)?.toDouble() ?? 0.0;
          _totalAvgMh = (data["total_avg"] as num?)?.toDouble() ?? 0.0;
          _totalHashes = (data["total_hashes"] as num?)?.toInt() ?? 0;
          _acceptedShares = (data["accepted"] as num?)?.toInt() ?? 0;
          _rejectedShares = (data["rejected"] as num?)?.toInt() ?? 0;
          _difficulty = (data["difficulty"] as num?)?.toDouble() ?? 1.0;
          _jobId = data["job_id"] as String? ?? "-";
          _threads = (data["threads"] as num?)?.toInt() ?? _threads;

          if (data["gpu_enabled"] != null) {
            _gpuEnabled = data["gpu_enabled"] as bool;
          }
          if (data["gpu_name"] != null && (data["gpu_name"] as String).isNotEmpty) {
            _gpuName = data["gpu_name"] as String;
          }
          if (data["mining_mode"] != null) {
            _isRpcMode = (data["mining_mode"] as String) == "btcrpc";
          }

          final logsList = data["logs"] as List<dynamic>?;
          if (logsList != null) {
            _logs.clear();
            for (final l in logsList) {
              _logs.add(l.toString());
            }
          }
          _recordHashrate(_liveMh);
        } else if (type == "telemetry") {
          _liveMh = (data["live_mh"] as num?)?.toDouble() ?? _liveMh;
          _cpuMh = (data["cpu_mh"] as num?)?.toDouble() ?? _cpuMh;
          _gpuMh = (data["gpu_mh"] as num?)?.toDouble() ?? _gpuMh;
          _avg5sMh = (data["avg_5s"] as num?)?.toDouble() ?? _avg5sMh;
          _totalAvgMh = (data["total_avg"] as num?)?.toDouble() ?? _totalAvgMh;
          _totalHashes = (data["total_hashes"] as num?)?.toInt() ?? _totalHashes;
          _acceptedShares = (data["accepted"] as num?)?.toInt() ?? _acceptedShares;
          _rejectedShares = (data["rejected"] as num?)?.toInt() ?? _rejectedShares;
          _difficulty = (data["difficulty"] as num?)?.toDouble() ?? _difficulty;
          _jobId = data["job_id"] as String? ?? _jobId;
          _isMining = data["is_mining"] as bool? ?? _isMining;

          if (data["gpu_name"] != null && (data["gpu_name"] as String).isNotEmpty) {
            _gpuName = data["gpu_name"] as String;
          }
          if (data["mining_mode"] != null) {
            _isRpcMode = (data["mining_mode"] as String) == "btcrpc";
          }

          _recordHashrate(_liveMh);
        } else if (type == "gpu_info") {
          if (data["gpu_name"] != null) {
            _gpuName = data["gpu_name"] as String;
          }
        } else if (type == "status") {
          _isMining = data["is_mining"] as bool? ?? _isMining;
          if (data["gpu_enabled"] != null) {
            _gpuEnabled = data["gpu_enabled"] as bool;
          }
          if (data["mining_mode"] != null) {
            _isRpcMode = (data["mining_mode"] as String) == "btcrpc";
          }
        } else if (type == "accepted") {
          _acceptedShares = (data["accepted"] as num?)?.toInt() ?? _acceptedShares;
          _triggerShareChime();
        } else if (type == "difficulty") {
          _difficulty = (data["difficulty"] as num?)?.toDouble() ?? _difficulty;
        } else if (type == "job") {
          _jobId = data["job_id"] as String? ?? _jobId;
        } else if (type == "log") {
          final line = data["line"] as String? ?? "";
          if (_logs.length > 1000) _logs.removeAt(0);
          _logs.add(line);
        }
      });

      if (_autoScroll && _logScrollController.hasClients) {
        WidgetsBinding.instance.addPostFrameCallback((_) {
          if (_logScrollController.hasClients) {
            _logScrollController.animateTo(
              _logScrollController.position.maxScrollExtent,
              duration: const Duration(milliseconds: 150),
              curve: Curves.easeOut,
            );
          }
        });
      }
    } catch (_) {}
  }

  void _recordHashrate(double mh) {
    _hashrateHistory.removeAt(0);
    _hashrateHistory.add(mh);
  }

  void _toggleMining() {
    if (!_isConnected) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(
          content: Text("Not connected to GUI bridge daemon. Waiting to reconnect..."),
          backgroundColor: Colors.redAccent,
        ),
      );
      return;
    }

    if (_isMining) {
      _channel?.sink.add(jsonEncode({"action": "stop"}));
    } else {
      final port = int.tryParse(_portController.text.trim()) ?? 3333;
      _channel?.sink.add(jsonEncode({
        "action": "start",
        "mode": _isRpcMode ? "btcrpc" : "stratum",
        "enable_gpu": _gpuEnabled,
        "threads": _threads,
        "pool": _poolController.text.trim(),
        "port": port,
        "user": _isRpcMode ? _rpcAddressController.text.trim() : _userController.text.trim(),
        "password": _passController.text.trim(),
        "rpc_url": _rpcUrlController.text.trim(),
        "rpc_user": _rpcUserController.text.trim(),
        "rpc_password": _rpcPassController.text.trim(),
      }));
    }
  }

  String _formatNumber(int n) {
    final s = n.toString();
    final buffer = StringBuffer();
    int count = 0;
    for (int i = s.length - 1; i >= 0; i--) {
      buffer.write(s[i]);
      count++;
      if (count % 3 == 0 && i != 0) {
        buffer.write(',');
      }
    }
    return buffer.toString().split('').reversed.join('');
  }

  @override
  Widget build(BuildContext context) {
    final screenWidth = MediaQuery.of(context).size.width;
    final isWide = screenWidth >= 1060;

    return Scaffold(
      appBar: _buildTopBar(),
      body: Container(
        decoration: const BoxDecoration(
          gradient: LinearGradient(
            begin: Alignment.topLeft,
            end: Alignment.bottomRight,
            colors: [Color(0xFF0D1117), Color(0xFF131821), Color(0xFF0D1117)],
          ),
        ),
        child: Padding(
          padding: const EdgeInsets.all(16.0),
          child: isWide ? _buildWideLayout() : _buildMobileLayout(),
        ),
      ),
    );
  }

  PreferredSizeWidget _buildTopBar() {
    return AppBar(
      backgroundColor: const Color(0xFF161B22),
      elevation: 0,
      title: Row(
        children: [
          Container(
            padding: const EdgeInsets.all(6),
            decoration: const BoxDecoration(
              shape: BoxShape.circle,
              gradient: LinearGradient(
                colors: [Color(0xFFFF9900), Color(0xFFF7931A)],
              ),
            ),
            child: const Text(
              '₿',
              style: TextStyle(
                color: Colors.white,
                fontSize: 18,
                fontWeight: FontWeight.bold,
              ),
            ),
          ),
          const SizedBox(width: 12),
          Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              const Text(
                'BITCOIN HYBRID MINER (SHA-NI + OPENCL)',
                style: TextStyle(
                  fontSize: 15,
                  fontWeight: FontWeight.bold,
                  letterSpacing: 0.8,
                  color: Colors.white,
                ),
              ),
              Text(
                'Dual-Transform Midstate • Intel Core i5 & ${_gpuName.split('(').first.trim()}',
                style: const TextStyle(fontSize: 11, color: Colors.white54),
              ),
            ],
          ),
        ],
      ),
      actions: [
        // Mode indicator
        Container(
          margin: const EdgeInsets.symmetric(vertical: 12, horizontal: 4),
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
          decoration: BoxDecoration(
            color: _isRpcMode ? const Color(0xFFBC8CFF).withValues(alpha: 0.18) : const Color(0xFF58A6FF).withValues(alpha: 0.18),
            borderRadius: BorderRadius.circular(16),
            border: Border.all(
              color: _isRpcMode ? const Color(0xFFBC8CFF) : const Color(0xFF58A6FF),
              width: 1,
            ),
          ),
          child: Text(
            _isRpcMode ? "BITCOIN CORE RPC" : "STRATUM POOL",
            style: TextStyle(
              fontSize: 11,
              fontWeight: FontWeight.bold,
              color: _isRpcMode ? const Color(0xFFD2A8FF) : const Color(0xFF79C0FF),
            ),
          ),
        ),

        // Bridge Status
        Container(
          margin: const EdgeInsets.symmetric(vertical: 12, horizontal: 4),
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
          decoration: BoxDecoration(
            color: _isConnected ? const Color(0xFF238636).withValues(alpha: 0.2) : const Color(0xFFDA3633).withValues(alpha: 0.2),
            borderRadius: BorderRadius.circular(16),
            border: Border.all(
              color: _isConnected ? const Color(0xFF2EA043) : const Color(0xFFF85149),
              width: 1,
            ),
          ),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Container(
                width: 8,
                height: 8,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: _isConnected ? const Color(0xFF3FB950) : const Color(0xFFF85149),
                ),
              ),
              const SizedBox(width: 6),
              Text(
                _isConnected ? "BRIDGE ONLINE" : "BRIDGE OFFLINE",
                style: TextStyle(
                  fontSize: 11,
                  fontWeight: FontWeight.w600,
                  color: _isConnected ? const Color(0xFF3FB950) : const Color(0xFFF85149),
                ),
              ),
            ],
          ),
        ),

        // Mining Status Indicator
        Container(
          margin: const EdgeInsets.only(right: 16, top: 12, bottom: 12, left: 4),
          padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 4),
          decoration: BoxDecoration(
            color: _isMining ? const Color(0xFFF7931A).withValues(alpha: 0.2) : Colors.white10,
            borderRadius: BorderRadius.circular(16),
            border: Border.all(
              color: _isMining ? const Color(0xFFF7931A) : Colors.white24,
              width: 1,
            ),
          ),
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              ScaleTransition(
                scale: _isMining ? _pulseAnimation : const AlwaysStoppedAnimation(1.0),
                child: Container(
                  width: 8,
                  height: 8,
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: _isMining ? const Color(0xFFF7931A) : Colors.white38,
                  ),
                ),
              ),
              const SizedBox(width: 6),
              Text(
                _isMining ? "MINING ACTIVE" : "IDLE",
                style: TextStyle(
                  fontSize: 11,
                  fontWeight: FontWeight.bold,
                  color: _isMining ? const Color(0xFFF7931A) : Colors.white60,
                ),
              ),
            ],
          ),
        ),
      ],
    );
  }

  Widget _buildWideLayout() {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        // Left Column: Controls & Configuration (width 410)
        SizedBox(
          width: 410,
          child: SingleChildScrollView(
            child: Column(
              children: [
                _buildControlsCard(),
                const SizedBox(height: 16),
                _buildActionCard(),
              ],
            ),
          ),
        ),
        const SizedBox(width: 16),
        // Right Column: Telemetry & Terminal
        Expanded(
          child: Column(
            children: [
              _buildTelemetrySection(),
              const SizedBox(height: 16),
              Expanded(child: _buildLogConsoleCard()),
            ],
          ),
        ),
      ],
    );
  }

  Widget _buildMobileLayout() {
    return SingleChildScrollView(
      child: Column(
        children: [
          _buildActionCard(),
          const SizedBox(height: 16),
          _buildTelemetrySection(),
          const SizedBox(height: 16),
          _buildControlsCard(),
          const SizedBox(height: 16),
          SizedBox(
            height: 380,
            child: _buildLogConsoleCard(),
          ),
        ],
      ),
    );
  }

  Widget _buildControlsCard() {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: const Color(0xFF30363D)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          // Mining Mode Switcher Tabs
          Container(
            padding: const EdgeInsets.all(3),
            decoration: BoxDecoration(
              color: const Color(0xFF0D1117),
              borderRadius: BorderRadius.circular(8),
              border: Border.all(color: const Color(0xFF30363D)),
            ),
            child: Row(
              children: [
                Expanded(
                  child: InkWell(
                    onTap: _isMining ? null : () => setState(() => _isRpcMode = false),
                    child: Container(
                      padding: const EdgeInsets.symmetric(vertical: 8),
                      decoration: BoxDecoration(
                        color: !_isRpcMode ? const Color(0xFFF7931A).withValues(alpha: 0.25) : Colors.transparent,
                        borderRadius: BorderRadius.circular(6),
                        border: Border.all(
                          color: !_isRpcMode ? const Color(0xFFF7931A) : Colors.transparent,
                        ),
                      ),
                      child: Center(
                        child: Row(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            Icon(Icons.hub_rounded, size: 14, color: !_isRpcMode ? const Color(0xFFF7931A) : Colors.white54),
                            const SizedBox(width: 6),
                            Text(
                              "Stratum Pool",
                              style: TextStyle(
                                fontSize: 12,
                                fontWeight: FontWeight.bold,
                                color: !_isRpcMode ? Colors.white : Colors.white54,
                              ),
                            ),
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
                const SizedBox(width: 4),
                Expanded(
                  child: InkWell(
                    onTap: _isMining ? null : () => setState(() => _isRpcMode = true),
                    child: Container(
                      padding: const EdgeInsets.symmetric(vertical: 8),
                      decoration: BoxDecoration(
                        color: _isRpcMode ? const Color(0xFFBC8CFF).withValues(alpha: 0.25) : Colors.transparent,
                        borderRadius: BorderRadius.circular(6),
                        border: Border.all(
                          color: _isRpcMode ? const Color(0xFFBC8CFF) : Colors.transparent,
                        ),
                      ),
                      child: Center(
                        child: Row(
                          mainAxisAlignment: MainAxisAlignment.center,
                          children: [
                            Icon(Icons.storage_rounded, size: 14, color: _isRpcMode ? const Color(0xFFD2A8FF) : Colors.white54),
                            const SizedBox(width: 6),
                            Text(
                              "Bitcoin Core RPC",
                              style: TextStyle(
                                fontSize: 12,
                                fontWeight: FontWeight.bold,
                                color: _isRpcMode ? Colors.white : Colors.white54,
                              ),
                            ),
                          ],
                        ),
                      ),
                    ),
                  ),
                ),
              ],
            ),
          ),
          const SizedBox(height: 16),

          if (!_isRpcMode) ...[
            // Pool Preset
            const Text("Stratum Pool Preset", style: TextStyle(fontSize: 11, color: Colors.white60)),
            const SizedBox(height: 6),
            Wrap(
              spacing: 8,
              runSpacing: 6,
              children: [
                ChoiceChip(
                  label: const Text("Solo CKPool"),
                  selected: _selectedPreset == PoolPreset.soloCkPool,
                  onSelected: (val) {
                    if (val && !_isMining) _applyPreset(PoolPreset.soloCkPool);
                  },
                  selectedColor: const Color(0xFFF7931A).withValues(alpha: 0.3),
                  side: BorderSide(
                    color: _selectedPreset == PoolPreset.soloCkPool ? const Color(0xFFF7931A) : Colors.white24,
                  ),
                ),
                ChoiceChip(
                  label: const Text("Binance Pool"),
                  selected: _selectedPreset == PoolPreset.binancePool,
                  onSelected: (val) {
                    if (val && !_isMining) _applyPreset(PoolPreset.binancePool);
                  },
                  selectedColor: const Color(0xFFF7931A).withValues(alpha: 0.3),
                  side: BorderSide(
                    color: _selectedPreset == PoolPreset.binancePool ? const Color(0xFFF7931A) : Colors.white24,
                  ),
                ),
                ChoiceChip(
                  label: const Text("Custom"),
                  selected: _selectedPreset == PoolPreset.custom,
                  onSelected: (val) {
                    if (val && !_isMining) _applyPreset(PoolPreset.custom);
                  },
                  selectedColor: const Color(0xFFF7931A).withValues(alpha: 0.3),
                  side: BorderSide(
                    color: _selectedPreset == PoolPreset.custom ? const Color(0xFFF7931A) : Colors.white24,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 14),

            // Pool Host & Port
            Row(
              children: [
                Expanded(
                  flex: 3,
                  child: _buildTextField(
                    label: "Stratum Host",
                    controller: _poolController,
                    enabled: !_isMining,
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  flex: 1,
                  child: _buildTextField(
                    label: "Port",
                    controller: _portController,
                    enabled: !_isMining,
                    isNumber: true,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 12),

            // Username / Wallet Address
            _buildTextField(
              label: "Stratum User / Worker / BTC Address",
              controller: _userController,
              enabled: !_isMining,
            ),
            const SizedBox(height: 12),

            // Password
            _buildTextField(
              label: "Password",
              controller: _passController,
              enabled: !_isMining,
            ),
          ] else ...[
            // Bitcoin Core RPC Solo Inputs
            Container(
              padding: const EdgeInsets.all(10),
              margin: const EdgeInsets.only(bottom: 12),
              decoration: BoxDecoration(
                color: const Color(0xFFBC8CFF).withValues(alpha: 0.1),
                borderRadius: BorderRadius.circular(8),
                border: Border.all(color: const Color(0xFFBC8CFF).withValues(alpha: 0.3)),
              ),
              child: const Row(
                children: [
                  Icon(Icons.info_outline_rounded, size: 16, color: Color(0xFFD2A8FF)),
                  SizedBox(width: 8),
                  Expanded(
                    child: Text(
                      "Solo mode queries local node via getblocktemplate (BIP22/23) & submits blocks directly.",
                      style: TextStyle(fontSize: 11, color: Colors.white70),
                    ),
                  ),
                ],
              ),
            ),
            _buildTextField(
              label: "Bitcoin Core RPC URL",
              controller: _rpcUrlController,
              enabled: !_isMining,
            ),
            const SizedBox(height: 10),
            Row(
              children: [
                Expanded(
                  child: _buildTextField(
                    label: "RPC Username",
                    controller: _rpcUserController,
                    enabled: !_isMining,
                  ),
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: _buildTextField(
                    label: "RPC Password",
                    controller: _rpcPassController,
                    enabled: !_isMining,
                    isPassword: true,
                  ),
                ),
              ],
            ),
            const SizedBox(height: 10),
            _buildTextField(
              label: "Block Reward Payout Address",
              controller: _rpcAddressController,
              enabled: !_isMining,
            ),
          ],

          const SizedBox(height: 18),
          const Divider(color: Color(0xFF30363D), height: 1),
          const SizedBox(height: 14),

          // Hardware Engine Configurations
          const Row(
            children: [
              Icon(Icons.memory_rounded, color: Color(0xFF58A6FF), size: 16),
              SizedBox(width: 6),
              Text(
                "HARDWARE MINING ENGINES",
                style: TextStyle(fontSize: 11, fontWeight: FontWeight.bold, letterSpacing: 0.5, color: Colors.white70),
              ),
            ],
          ),
          const SizedBox(height: 10),

          // CPU Engine Slider
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              const Text("CPU Worker Threads (SHA-NI)", style: TextStyle(fontSize: 12, color: Colors.white60)),
              Container(
                padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
                decoration: BoxDecoration(
                  color: const Color(0xFF58A6FF).withValues(alpha: 0.15),
                  borderRadius: BorderRadius.circular(6),
                  border: Border.all(color: const Color(0xFF58A6FF).withValues(alpha: 0.4)),
                ),
                child: Text(
                  "$_threads Threads",
                  style: const TextStyle(
                    fontSize: 12,
                    fontWeight: FontWeight.bold,
                    color: Color(0xFF58A6FF),
                  ),
                ),
              ),
            ],
          ),
          SliderTheme(
            data: SliderTheme.of(context).copyWith(
              activeTrackColor: const Color(0xFF58A6FF),
              thumbColor: const Color(0xFF58A6FF),
              overlayColor: const Color(0xFF58A6FF).withValues(alpha: 0.2),
              valueIndicatorColor: const Color(0xFF58A6FF),
            ),
            child: Slider(
              value: _threads.toDouble(),
              min: 1,
              max: 8,
              divisions: 7,
              label: "$_threads",
              onChanged: _isMining
                  ? null
                  : (val) {
                      setState(() => _threads = val.round());
                    },
            ),
          ),

          // GPU Engine Toggle Switch Card
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
            decoration: BoxDecoration(
              color: const Color(0xFF0D1117),
              borderRadius: BorderRadius.circular(8),
              border: Border.all(
                color: _gpuEnabled ? const Color(0xFFBC8CFF).withValues(alpha: 0.4) : const Color(0xFF30363D),
              ),
            ),
            child: Row(
              children: [
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          Icon(
                            Icons.videogame_asset_rounded,
                            size: 15,
                            color: _gpuEnabled ? const Color(0xFFD2A8FF) : Colors.white38,
                          ),
                          const SizedBox(width: 6),
                          Text(
                            "OpenCL GPU Engine",
                            style: TextStyle(
                              fontSize: 12,
                              fontWeight: FontWeight.bold,
                              color: _gpuEnabled ? Colors.white : Colors.white60,
                            ),
                          ),
                        ],
                      ),
                      const SizedBox(height: 2),
                      Text(
                        _gpuName,
                        style: const TextStyle(fontSize: 10, color: Colors.white38),
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                      ),
                      const SizedBox(height: 1),
                      Text(
                        _gpuEnabled
                            ? "Nonce Partition: CPU [0x0..0x7FFFFFFF] | GPU [0x80000000..0xFFFFFFFF]"
                            : "GPU Worker Disabled (CPU-Only Mode)",
                        style: TextStyle(
                          fontSize: 9.5,
                          color: _gpuEnabled ? const Color(0xFF3FB950) : Colors.white38,
                        ),
                      ),
                    ],
                  ),
                ),
                Switch(
                  value: _gpuEnabled,
                  onChanged: _isMining ? null : (val) => setState(() => _gpuEnabled = val),
                  activeThumbColor: const Color(0xFFD2A8FF),
                  activeTrackColor: const Color(0xFF8957E5),
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }

  Widget _buildActionCard() {
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: const Color(0xFF30363D)),
      ),
      child: Column(
        children: [
          SizedBox(
            width: double.infinity,
            height: 52,
            child: ElevatedButton(
              style: ElevatedButton.styleFrom(
                backgroundColor: _isMining ? const Color(0xFFDA3633) : const Color(0xFF238636),
                foregroundColor: Colors.white,
                shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                elevation: 4,
              ),
              onPressed: _toggleMining,
              child: Row(
                mainAxisAlignment: MainAxisAlignment.center,
                children: [
                  Icon(_isMining ? Icons.stop_rounded : Icons.play_arrow_rounded, size: 26),
                  const SizedBox(width: 8),
                  Text(
                    _isMining ? "STOP MINING RIG" : "START HYBRID MINER",
                    style: const TextStyle(
                      fontSize: 16,
                      fontWeight: FontWeight.bold,
                      letterSpacing: 1.0,
                    ),
                  ),
                ],
              ),
            ),
          ),
          const SizedBox(height: 10),
          Text(
            _isMining
                ? "Mining active on $_threads CPU threads${_gpuEnabled ? " + Intel Iris Xe GPU" : ""}. Click to halt worker engines."
                : "Select mode and parameters above, then launch the hybrid mining rig.",
            textAlign: TextAlign.center,
            style: const TextStyle(fontSize: 11, color: Colors.white54),
          ),
        ],
      ),
    );
  }

  Widget _buildTextField({
    required String label,
    required TextEditingController controller,
    required bool enabled,
    bool isNumber = false,
    bool isPassword = false,
  }) {
    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text(label, style: const TextStyle(fontSize: 11, color: Colors.white60)),
        const SizedBox(height: 4),
        TextField(
          controller: controller,
          enabled: enabled,
          obscureText: isPassword,
          keyboardType: isNumber ? TextInputType.number : TextInputType.text,
          style: const TextStyle(fontSize: 13, color: Colors.white),
          decoration: InputDecoration(
            isDense: true,
            contentPadding: const EdgeInsets.symmetric(horizontal: 10, vertical: 10),
            filled: true,
            fillColor: enabled ? const Color(0xFF0D1117) : const Color(0xFF13171F),
            enabledBorder: OutlineInputBorder(
              borderRadius: BorderRadius.circular(6),
              borderSide: const BorderSide(color: Color(0xFF30363D)),
            ),
            focusedBorder: OutlineInputBorder(
              borderRadius: BorderRadius.circular(6),
              borderSide: const BorderSide(color: Color(0xFFF7931A)),
            ),
            disabledBorder: OutlineInputBorder(
              borderRadius: BorderRadius.circular(6),
              borderSide: const BorderSide(color: Color(0xFF21262D)),
            ),
          ),
        ),
      ],
    );
  }

  Widget _buildTelemetrySection() {
    return Column(
      children: [
        // Hero Aggregate Hashrate Display + Split CPU / GPU chips + Real-time Line Graph
        Container(
          padding: const EdgeInsets.all(16),
          decoration: BoxDecoration(
            color: const Color(0xFF161B22),
            borderRadius: BorderRadius.circular(12),
            border: Border.all(color: const Color(0xFF30363D)),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Row(
                mainAxisAlignment: MainAxisAlignment.spaceBetween,
                children: [
                  const Row(
                    children: [
                      Icon(Icons.speed_rounded, color: Color(0xFF58A6FF), size: 20),
                      SizedBox(width: 8),
                      Text(
                        'AGGREGATE RIG HASHRATE',
                        style: TextStyle(
                          fontSize: 13,
                          fontWeight: FontWeight.bold,
                          letterSpacing: 0.5,
                          color: Colors.white70,
                        ),
                      ),
                    ],
                  ),
                  Row(
                    children: [
                      _buildMiniBadge("5s Avg: ${_avg5sMh.toStringAsFixed(2)} MH/s", const Color(0xFF58A6FF)),
                      const SizedBox(width: 8),
                      _buildMiniBadge("Session: ${_totalAvgMh.toStringAsFixed(2)} MH/s", const Color(0xFFBC8CFF)),
                    ],
                  ),
                ],
              ),
              const SizedBox(height: 12),
              Row(
                crossAxisAlignment: CrossAxisAlignment.baseline,
                textBaseline: TextBaseline.alphabetic,
                children: [
                  Text(
                    _liveMh.toStringAsFixed(2),
                    style: const TextStyle(
                      fontSize: 48,
                      fontWeight: FontWeight.w900,
                      color: Colors.white,
                      letterSpacing: -1,
                    ),
                  ),
                  const SizedBox(width: 8),
                  const Text(
                    'MH/s',
                    style: TextStyle(
                      fontSize: 20,
                      fontWeight: FontWeight.bold,
                      color: Color(0xFFF7931A),
                    ),
                  ),
                  const Spacer(),
                  Text(
                    "Job: ${_jobId.length > 12 ? "${_jobId.substring(0, 12)}..." : _jobId}",
                    style: const TextStyle(fontSize: 12, fontFamily: 'monospace', color: Colors.white38),
                  ),
                ],
              ),
              const SizedBox(height: 10),

              // Split Telemetry Chips: CPU (SHA-NI) and GPU (Iris Xe)
              Row(
                children: [
                  Expanded(
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 7),
                      decoration: BoxDecoration(
                        color: const Color(0xFF0D1117),
                        borderRadius: BorderRadius.circular(8),
                        border: Border.all(color: const Color(0xFF58A6FF).withValues(alpha: 0.4)),
                      ),
                      child: Row(
                        children: [
                          Container(
                            width: 7,
                            height: 7,
                            decoration: const BoxDecoration(
                              shape: BoxShape.circle,
                              color: Color(0xFF58A6FF),
                            ),
                          ),
                          const SizedBox(width: 6),
                          const Text(
                            "⚡ CPU (SHA-NI): ",
                            style: TextStyle(fontSize: 11, color: Colors.white60),
                          ),
                          Text(
                            "${_cpuMh.toStringAsFixed(2)} MH/s",
                            style: const TextStyle(fontSize: 12, fontWeight: FontWeight.bold, color: Color(0xFF79C0FF)),
                          ),
                        ],
                      ),
                    ),
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: Container(
                      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 7),
                      decoration: BoxDecoration(
                        color: const Color(0xFF0D1117),
                        borderRadius: BorderRadius.circular(8),
                        border: Border.all(
                          color: _gpuEnabled ? const Color(0xFFBC8CFF).withValues(alpha: 0.4) : const Color(0xFF30363D),
                        ),
                      ),
                      child: Row(
                        children: [
                          Container(
                            width: 7,
                            height: 7,
                            decoration: BoxDecoration(
                              shape: BoxShape.circle,
                              color: _gpuEnabled ? const Color(0xFFD2A8FF) : Colors.white24,
                            ),
                          ),
                          const SizedBox(width: 6),
                          const Text(
                            "🚀 GPU (Iris Xe): ",
                            style: TextStyle(fontSize: 11, color: Colors.white60),
                          ),
                          Text(
                            _gpuEnabled ? "${_gpuMh.toStringAsFixed(2)} MH/s" : "Disabled",
                            style: TextStyle(
                              fontSize: 12,
                              fontWeight: FontWeight.bold,
                              color: _gpuEnabled ? const Color(0xFFD2A8FF) : Colors.white38,
                            ),
                          ),
                        ],
                      ),
                    ),
                  ),
                ],
              ),
              const SizedBox(height: 14),

              // Hashrate Canvas Graph
              SizedBox(
                height: 100,
                width: double.infinity,
                child: CustomPaint(
                  painter: HashrateChartPainter(
                    history: _hashrateHistory,
                    lineColor: const Color(0xFFF7931A),
                    fillColor: const Color(0xFFF7931A).withValues(alpha: 0.15),
                    avg5s: _avg5sMh,
                  ),
                ),
              ),
            ],
          ),
        ),
        const SizedBox(height: 16),

        // 4 Stat Cards Row
        Row(
          children: [
            Expanded(
              child: _buildMetricCard(
                title: "ACCEPTED SHARES",
                value: "$_acceptedShares",
                subtitle: _acceptedShares + _rejectedShares > 0
                    ? "${((_acceptedShares / (_acceptedShares + _rejectedShares)) * 100).toStringAsFixed(1)}% valid"
                    : "0.0% valid",
                icon: Icons.check_circle_rounded,
                iconColor: const Color(0xFF3FB950),
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _buildMetricCard(
                title: "REJECTED SHARES",
                value: "$_rejectedShares",
                subtitle: _acceptedShares + _rejectedShares > 0
                    ? "${((_rejectedShares / (_acceptedShares + _rejectedShares)) * 100).toStringAsFixed(1)}% rejected"
                    : "0.0% rejected",
                icon: Icons.cancel_rounded,
                iconColor: const Color(0xFFF85149),
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _buildMetricCard(
                title: "TOTAL HASHES",
                value: _formatNumber(_totalHashes),
                subtitle: "Computed",
                icon: Icons.memory_rounded,
                iconColor: const Color(0xFF58A6FF),
              ),
            ),
            const SizedBox(width: 12),
            Expanded(
              child: _buildMetricCard(
                title: _isRpcMode ? "NETWORK DIFF" : "POOL DIFFICULTY",
                value: _difficulty >= 1000 ? "${(_difficulty / 1000).toStringAsFixed(1)}k" : _difficulty.toStringAsFixed(2),
                subtitle: _isRpcMode ? "Block Target Diff" : "Target Share Diff",
                icon: Icons.military_tech_rounded,
                iconColor: const Color(0xFFBC8CFF),
              ),
            ),
          ],
        ),
      ],
    );
  }

  Widget _buildMiniBadge(String text, Color color) {
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.15),
        borderRadius: BorderRadius.circular(6),
        border: Border.all(color: color.withValues(alpha: 0.3)),
      ),
      child: Text(
        text,
        style: TextStyle(fontSize: 11, fontWeight: FontWeight.w600, color: color),
      ),
    );
  }

  Widget _buildMetricCard({
    required String title,
    required String value,
    required String subtitle,
    required IconData icon,
    required Color iconColor,
  }) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: const Color(0xFF161B22),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: const Color(0xFF30363D)),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            mainAxisAlignment: MainAxisAlignment.spaceBetween,
            children: [
              Text(
                title,
                style: const TextStyle(fontSize: 10, fontWeight: FontWeight.bold, color: Colors.white54),
              ),
              Icon(icon, color: iconColor, size: 16),
            ],
          ),
          const SizedBox(height: 6),
          FittedBox(
            fit: BoxFit.scaleDown,
            child: Text(
              value,
              style: const TextStyle(fontSize: 22, fontWeight: FontWeight.bold, color: Colors.white),
            ),
          ),
          const SizedBox(height: 2),
          Text(
            subtitle,
            style: TextStyle(fontSize: 11, color: iconColor.withValues(alpha: 0.9)),
          ),
        ],
      ),
    );
  }

  Widget _buildLogConsoleCard() {
    return Container(
      decoration: BoxDecoration(
        color: const Color(0xFF0A0D12),
        borderRadius: BorderRadius.circular(12),
        border: Border.all(color: const Color(0xFF30363D)),
      ),
      child: Column(
        children: [
          // Console Header
          Container(
            padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
            decoration: const BoxDecoration(
              color: Color(0xFF161B22),
              borderRadius: BorderRadius.only(topLeft: Radius.circular(12), topRight: Radius.circular(12)),
            ),
            child: Row(
              children: [
                const Icon(Icons.terminal_rounded, size: 16, color: Colors.white60),
                const SizedBox(width: 8),
                Text(
                  _isRpcMode ? 'BITCOIN CORE RPC ENGINE CONSOLE' : 'STRATUM ENGINE CONSOLE',
                  style: const TextStyle(fontSize: 12, fontWeight: FontWeight.bold, letterSpacing: 0.5, color: Colors.white70),
                ),
                const Spacer(),
                // Auto-scroll Switch
                Row(
                  children: [
                    const Text("Auto-Scroll", style: TextStyle(fontSize: 11, color: Colors.white54)),
                    const SizedBox(width: 4),
                    Switch(
                      value: _autoScroll,
                      onChanged: (val) => setState(() => _autoScroll = val),
                      activeTrackColor: const Color(0xFFF7931A),
                      materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    ),
                  ],
                ),
                const SizedBox(width: 8),
                // Clear button
                IconButton(
                  icon: const Icon(Icons.clear_all_rounded, size: 18, color: Colors.white54),
                  tooltip: "Clear Console",
                  onPressed: () => setState(() => _logs.clear()),
                ),
              ],
            ),
          ),
          // Console Log Lines
          Expanded(
            child: _logs.isEmpty
                ? const Center(
                    child: Text(
                      "No log events yet. Start the miner to view engine communication.",
                      style: TextStyle(fontSize: 12, color: Colors.white24, fontStyle: FontStyle.italic),
                    ),
                  )
                : ListView.builder(
                    controller: _logScrollController,
                    padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 8),
                    itemCount: _logs.length,
                    itemBuilder: (context, index) {
                      final line = _logs[index];
                      Color color = Colors.white70;
                      if (line.contains("SHARE ACCEPTED") || line.contains("BLOCK ACCEPTED") || line.contains("[+]")) {
                        color = const Color(0xFF3FB950);
                      } else if (line.contains("SHARE REJECTED") || line.contains("[-]")) {
                        color = const Color(0xFFF85149);
                      } else if (line.contains("GPU") || line.contains("OpenCL") || line.contains("Iris Xe")) {
                        color = const Color(0xFFD2A8FF);
                      } else if (line.contains("[*]")) {
                        color = const Color(0xFF58A6FF);
                      } else if (line.contains("Difficulty") || line.contains("JobID") || line.contains("Block #")) {
                        color = const Color(0xFFBC8CFF);
                      }

                      return Padding(
                        padding: const EdgeInsets.symmetric(vertical: 1.5),
                        child: Text(
                          line,
                          style: TextStyle(
                            fontFamily: 'Consolas',
                            fontSize: 11.5,
                            color: color,
                            height: 1.3,
                          ),
                        ),
                      );
                    },
                  ),
          ),
        ],
      ),
    );
  }
}

class HashrateChartPainter extends CustomPainter {
  final List<double> history;
  final Color lineColor;
  final Color fillColor;
  final double avg5s;

  HashrateChartPainter({
    required this.history,
    required this.lineColor,
    required this.fillColor,
    required this.avg5s,
  });

  @override
  void paint(Canvas canvas, Size size) {
    if (history.isEmpty) return;

    double maxVal = history.reduce(math.max);
    if (maxVal < 1.0) maxVal = 5.0;
    maxVal *= 1.25; // headroom

    final gridPaint = Paint()
      ..color = Colors.white.withValues(alpha: 0.06)
      ..strokeWidth = 1.0;

    // Draw horizontal grid lines
    for (int i = 1; i <= 3; i++) {
      final y = size.height * (i / 4.0);
      canvas.drawLine(Offset(0, y), Offset(size.width, y), gridPaint);
    }

    final path = Path();
    final fillPath = Path();

    final stepX = size.width / (history.length - 1);

    for (int i = 0; i < history.length; i++) {
      final x = i * stepX;
      final y = size.height - (history[i] / maxVal) * size.height;

      if (i == 0) {
        path.moveTo(x, y);
        fillPath.moveTo(x, size.height);
        fillPath.lineTo(x, y);
      } else {
        path.lineTo(x, y);
        fillPath.lineTo(x, y);
      }
    }

    fillPath.lineTo(size.width, size.height);
    fillPath.close();

    // Fill under curve
    final areaPaint = Paint()
      ..shader = LinearGradient(
        begin: Alignment.topCenter,
        end: Alignment.bottomCenter,
        colors: [fillColor, fillColor.withValues(alpha: 0.0)],
      ).createShader(Rect.fromLTWH(0, 0, size.width, size.height));

    canvas.drawPath(fillPath, areaPaint);

    // Stroke line
    final linePaint = Paint()
      ..color = lineColor
      ..strokeWidth = 2.2
      ..style = PaintingStyle.stroke
      ..strokeCap = StrokeCap.round
      ..strokeJoin = StrokeJoin.round;

    canvas.drawPath(path, linePaint);

    // Current point circle
    if (history.isNotEmpty) {
      final lastX = size.width;
      final lastY = size.height - (history.last / maxVal) * size.height;
      final dotPaint = Paint()..color = Colors.white;
      canvas.drawCircle(Offset(lastX, lastY), 3.5, dotPaint);
    }
  }

  @override
  bool shouldRepaint(covariant HashrateChartPainter oldDelegate) => true;
}
