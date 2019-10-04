// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'dart:async';
import 'dart:io';

import 'package:logging/logging.dart';
import 'package:meta/meta.dart';
import 'package:retry/retry.dart';

import 'exceptions.dart';

class Ssh {
  static const _sshUser = 'fuchsia';

  final _log = Logger('ssh');

  /// Authority (IP, hostname, etc.) of the device under test.
  final String target;

  /// Path to an SSH key file. For in-tree Fuchsia development, this can be
  /// the resolved path of `//.ssh/pkey`.
  final String sshKeyPath;

  /// Builds an SSH object that uses the credentials from a file.
  Ssh(this.target, this.sshKeyPath)
      : assert(target != null && target.isNotEmpty),
        assert(sshKeyPath != null && sshKeyPath.isNotEmpty) {
    _log.info('SSH key path: $sshKeyPath');
  }

  /// Builds an SSH object that uses the credentials from ssh-agent only.
  Ssh.useAgent(this.target) : sshKeyPath = null;

  /// Starts an ssh [Process], sending [cmd] to the target using ssh.
  Future<Process> start(String cmd,
      {ProcessStartMode mode = ProcessStartMode.normal}) {
    _log.fine('Running over ssh: $cmd');

    return Process.start('ssh', makeArgs(cmd),
        // If not run in a shell it doesn't seem like the PATH is searched.
        runInShell: true,
        mode: mode);
  }

  /// Runs the command given by [cmd] on the target using ssh.
  ///
  /// It can optionally send input via [stdin], and can optionally incrementally
  /// emit output via [stdoutConsumer] and [stderrConsumer].
  ///
  /// If the exit code is nonzero, diagnostic warnings are logged.
  Future<ProcessResult> runWithOutput(String cmd,
      {String stdin,
      StreamConsumer<String> stdoutConsumer,
      StreamConsumer<String> stderrConsumer}) async {
    final process = await start(cmd);

    if (stdin != null) {
      process.stdin.write(stdin);
    }

    final localStdoutAll = StringBuffer();
    final localStderrAll = StringBuffer();

    final localStdoutStream =
        process.stdout.transform(systemEncoding.decoder).map((String data) {
      localStdoutAll.write(data);
      return data;
    });
    final localStderrStream =
        process.stderr.transform(systemEncoding.decoder).map((String data) {
      localStderrAll.write(data);
      return data;
    });

    final Future<void> stdoutFuture = (stdoutConsumer != null)
        ? localStdoutStream.pipe(stdoutConsumer)
        : localStdoutStream.drain();
    final Future<void> stderrFuture = (stderrConsumer != null)
        ? localStderrStream.pipe(stderrConsumer)
        : localStderrStream.drain();

    Future<void> flushAndCloseStdin() async {
      // These two need to be sequenced in order.
      await process.stdin.flush();
      await process.stdin.close();
    }

    // This waits for stdin, stdout, stderr to all be done.  It's important that
    // we concurrently wait for stdin and stdout/stderr, to avoid potential
    // deadlock (especially if all three have lots of data).
    await Future.wait([
      flushAndCloseStdin(),
      stdoutFuture,
      stderrFuture,
    ]);

    final result = ProcessResult(
      process.pid,
      await process.exitCode,
      localStdoutAll.toString(),
      localStderrAll.toString(),
    );

    if (result.exitCode != 0) {
      _log
        ..warning('$cmd; exit code: ${result.exitCode}')
        ..warning(result.stdout)
        ..warning(result.stderr);
    }

    return result;
  }

  /// Runs the command given by [cmd] on the target using ssh.
  ///
  /// It can optionally send input via [stdin]. If the exit code is nonzero,
  /// diagnostic warnings are logged.
  Future<ProcessResult> run(String cmd, {String stdin}) async {
    return runWithOutput(cmd, stdin: stdin);
  }

  /// Forwards TCP connections from the local [port] to the DUT's [remotePort].
  ///
  /// If [port] is not provided, an unused port will be allocated.
  /// The return value is the local forwarded port, or [PortForwardException] is
  /// thrown in case of error.
  Future<int> forwardPort(
      {@required int remotePort, int port, int tries = 5}) async {
    port ??= await pickUnusedPort();
    _log.fine('Forwarding TCP port: localhost:$port -> $target:$remotePort');
    await retry(
      () => _forwardPort(remotePort, port),
      retryIf: (e) => e is PortForwardException,
      maxAttempts: tries,
    );
    return port;
  }

  /// Forwards a port to the DUT without retries.
  Future<void> _forwardPort(int remotePort, int port) async {
    final result = await Process.run(
        'ssh', makeForwardArgs(port, remotePort, cancel: false),
        runInShell: true);
    if (result.exitCode != 0) {
      throw PortForwardException(
          'localhost:$port',
          '$target:$remotePort',
          'Failed to initiate Port Forward. '
              'STDOUT: "${result.stdout}". STDERR: "${result.stderr}".');
    }
  }

  /// Cancels a TCP port forward.
  ///
  /// Completes to PortForwardException in case of failure.
  Future<void> cancelPortForward(
      {@required int remotePort, @required int port}) async {
    _log.fine('Canceling TCP port forward: '
        'localhost:$port -> $target:$remotePort');

    final result = await Process.run(
        'ssh', makeForwardArgs(port, remotePort, cancel: true),
        runInShell: true);
    if (result.exitCode != 0) {
      throw PortForwardException(
          'localhost:$port',
          '$target:$remotePort',
          'Failed to Cancel Port Forward. '
              'STDOUT: "${result.stdout}". STDERR: "${result.stderr}".');
    }
  }

  List<String> _makeBaseArgs() =>
      [
        // Don't check known_hosts.
        '-o', 'UserKnownHostsFile=/dev/null',
        // Auto add the fingerprint of remote host.
        '-o', 'StrictHostKeyChecking=no',
        // Timeout to connect, short so the logs can make sense.
        '-o', 'ConnectTimeout=2',
        // These five arguments allow ssh to reuse its connection.
        '-o', 'ControlPersist=yes',
        '-o', 'ControlMaster=auto',
        '-o', 'ControlPath=/tmp/fuchsia--%r@%h:%p',
        '-o', 'ServerAliveInterval=1',
        '-o', 'ServerAliveCountMax=1',
        // These two arguments determine the connection timeout,
        // in the case the ssh connection gets lost.
        // They say if the target doesn't respond within 10 seconds, six
        // times in a row, terminate the connection.
        '-o', 'ServerAliveInterval=10',
        '-o', 'ServerAliveCountMax=6',
        '$_sshUser@$target',
      ] +
      (sshKeyPath != null ? ['-i', sshKeyPath] : []);

  @visibleForTesting
  List<String> makeArgs(String cmd) => _makeBaseArgs() + [cmd];

  @visibleForTesting
  List<String> makeForwardArgs(int localPort, int remotePort,
          {bool cancel = false}) =>
      _makeBaseArgs() +
      [
        // Do Not run a command.
        '-N',
        // TCP port forward from local to remote.
        '-L', 'localhost:$localPort:localhost:$remotePort',
        // Forwarding with -O makes sure we are reusing the same connection.
        '-O', cancel ? 'cancel' : 'forward',
      ];

  /// Finds and returns an unused port on the test host in the local port range
  /// (see ip(7)).
  Future<int> pickUnusedPort() async {
    // Use bind to allocate an unused port, then unbind from that port to
    // make it available for use.
    final socket = await ServerSocket.bind('localhost', 0);
    final port = socket.port;
    await socket.close();

    return port;
  }
}
