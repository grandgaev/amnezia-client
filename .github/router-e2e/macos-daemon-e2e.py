#!/usr/bin/env python3
import ipaddress
import json
import os
from pathlib import Path
import re
import selectors
import signal
import socket
import subprocess
import sys
import time


def run(*args, check=True, timeout=30):
    return subprocess.run(args, capture_output=True, text=True, check=check, timeout=timeout)


def curl(url, *options):
    return run('sudo', '-u', run_as, 'curl', '-4', '-fsS', '--connect-timeout', '10',
               '--max-time', '20', *options, url).stdout.strip()


def route_interface(address, scope=None):
    args = ['route', '-n', 'get']
    if scope:
        args += ['-ifscope', scope]
    result = run(*args, address, check=False)
    match = re.search(r'interface:\s+(\S+)', result.stdout)
    return match.group(1) if match else None


def scoped_default_exists(scope):
    result = run('route', '-n', 'get', '-ifscope', scope, 'default', check=False)
    return 'IFSCOPE' in result.stdout


def management_addresses():
    addresses = set()
    for line in run('lsof', '-nP', '-iTCP', '-sTCP:ESTABLISHED', '-Fn', check=False).stdout.splitlines():
        if '->' not in line:
            continue
        candidate = line.rsplit('->', 1)[1].rsplit(':', 1)[0]
        try:
            if ipaddress.ip_address(candidate).version == 4:
                addresses.add(candidate)
        except ValueError:
            pass
    for domain in ('api.github.com', 'github.com', 'objects.githubusercontent.com',
                   'results-receiver.actions.githubusercontent.com'):
        try:
            addresses.update(info[4][0] for info in socket.getaddrinfo(domain, 443, socket.AF_INET))
        except OSError:
            pass
    return sorted(addresses)


class Daemon:
    def __init__(self):
        self.socket = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.socket.settimeout(45)
        self.socket.connect('/var/run/amneziavpn-test/daemon.socket')
        self.reader = self.socket.makefile('rb')

    def send(self, command):
        self.socket.sendall(json.dumps(command).encode() + b'\n')

    def wait(self, kind):
        while True:
            line = self.reader.readline()
            if not line:
                raise RuntimeError('Daemon closed the control socket')
            message = json.loads(line)
            if message.get('type') == 'backendFailure':
                raise RuntimeError(f"Daemon backend failed with code {message.get('errorCode')}")
            if message.get('type') == 'disconnected' and kind != 'disconnected':
                raise RuntimeError('Daemon rejected the test connection')
            if message.get('type') == kind:
                return message

    def disconnect(self):
        self.send({'type': 'deactivate'})
        self.wait('disconnected')
        self.send({'type': 'status'})
        if self.wait('status').get('connected'):
            raise RuntimeError('Daemon still reports a connected tunnel after deactivation')
        deadline = time.monotonic() + 10
        while not baseline_scoped and scoped_default_exists(uplink):
            if time.monotonic() >= deadline:
                raise RuntimeError('The daemon left its scoped default route after disconnect')
            time.sleep(.2)


def connect(routing, kill_switch):
    config = dict(credentials)
    config.update({
        'type': 'activate',
        'deviceIpv4Address': str(ipaddress.ip_interface(credentials['deviceIpv4Address'] + '/32')),
        'primaryDnsServer': '198.18.0.53',
        'secondaryDnsServer': '198.18.0.53',
        'allowedIPAddressRanges': [{'address': '0.0.0.0', 'range': 0, 'isIpv6': False}],
        'excludedAddresses': [credentials['serverIpv4AddrIn'], *management],
        'vpnDisabledApps': [], 'allowedDnsServers': [],
        'killSwitchOption': 'true' if kill_switch else 'false',
        'routingConfig': routing,
    })
    daemon.send(config)
    daemon.wait('connected')
    daemon.send({'type': 'status'})
    status = daemon.wait('status')
    if not status.get('connected'):
        raise RuntimeError('Daemon did not confirm the live tunnel')
    for address in {credentials['serverIpv4AddrIn'], *management}:
        if route_interface(address) != uplink:
            raise RuntimeError('A server or CI management exception no longer uses the physical interface')
    if kill_switch:
        required_anchors = {
            '100.blockAll': 'block drop out',
            '150.allowExcludedApps': r'pass out quick.*user (?:= )?(?:root|0)\b',
            '200.allowVPN': 'pass out',
            '250.blockIPv6': 'inet6',
            '310.blockDNS': 'block return out proto',
        }
        for anchor, expected in required_anchors.items():
            rules = run('pfctl', '-a', f'amn-test/{anchor}', '-s', 'rules').stdout
            if not re.search(expected, rules):
                raise RuntimeError(f'Kill switch anchor {anchor} is missing its expected rule')
    print(f'PASS: daemon connected, kill switch {kill_switch}', flush=True)


def dns(name, record='A'):
    return run('sudo', '-u', run_as, 'dig', '+time=5', '+tries=1', '+short',
               '@198.18.0.53', name, record).stdout.strip()


def direct_https(host, address):
    if route_interface(address) == uplink:
        raise RuntimeError('Direct target bypassed the router at the OS route layer')
    capture = subprocess.Popen(
        ['tcpdump', '-i', uplink, '-nn', '-l', '-c', '1',
         f'tcp and dst host {address} and dst port 443 and tcp[tcpflags] & tcp-syn != 0'],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, bufsize=0)
    try:
        with selectors.DefaultSelector() as selector:
            selector.register(capture.stderr, selectors.EVENT_READ)
            deadline = time.monotonic() + 5
            startup = b''
            while True:
                if not selector.select(max(0, deadline - time.monotonic())):
                    raise RuntimeError('Physical-interface capture did not become ready')
                startup += os.read(capture.stderr.fileno(), 4096)
                if b'listening on' in startup:
                    break
                if capture.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError('Physical-interface capture failed to start')
        curl(f'https://{host}/', '--resolve', f'{host}:443:{address}', '-o', '/dev/null')
        packets, _ = capture.communicate(timeout=5)
        if capture.returncode != 0 or f'{address}.443'.encode() not in packets:
            raise RuntimeError(f'No direct TCP packet to {host} was observed on the physical interface')
    finally:
        if capture.poll() is None:
            capture.terminate()
            capture.communicate(timeout=5)
    print(f'PASS: direct HTTPS {host}, physical-interface TCP SYN observed', flush=True)


if os.environ.get('GITHUB_ACTIONS') != 'true' or os.getuid() != 0:
    sys.exit('This test changes routes and must run as root on a disposable GitHub runner')

credentials = json.loads(Path(sys.argv[1]).read_text())
required_fields = ('privateKey', 'serverPublicKey', 'serverIpv4AddrIn',
                   'serverIpv4Gateway', 'deviceIpv4Address')
if any(not isinstance(credentials.get(field), str) or not credentials[field]
       for field in required_fields) or not isinstance(credentials.get('serverPort'), int):
    sys.exit('The isolated server configuration is missing required fields')
credentials['deviceIpv4Address'] = str(ipaddress.IPv4Interface(credentials['deviceIpv4Address']).ip)
for field in ('serverIpv4AddrIn', 'serverIpv4Gateway'):
    credentials[field] = str(ipaddress.IPv4Address(credentials[field]))
original_profile = json.loads(Path(sys.argv[2]).read_text())
run_as = sys.argv[3]
management = management_addresses()
uplink = route_interface('default')
if not uplink or uplink.startswith('utun'):
    sys.exit('The disposable runner has no physical default interface')
baseline_ip = curl('https://api.ipify.org')
if not run('dig', '+time=5', '+tries=1', '+short', 'example.com', 'A').stdout.strip():
    sys.exit('The block control name did not resolve before connecting')
baseline_scoped = scoped_default_exists(uplink)
print(f'Baseline physical interface {uplink}; management exceptions {len(management)}', flush=True)
daemon = Daemon()


def stop_on_signal(_signum, _frame):
    raise RuntimeError('Test deadline reached')


signal.signal(signal.SIGTERM, stop_on_signal)
signal.signal(signal.SIGALRM, stop_on_signal)
signal.alarm(240)
try:
    synthetic = {
        'version': 1, 'defaultAction': 'proxy', 'bypass': 'interface',
        'rejectDirectIpv6': True, 'rejectProxyIpv6': True,
        'rules': [
            {'action': 'block', 'domains': ['full:example.com']},
            {'action': 'direct', 'domains': ['full:api.ipify.org', 'domain:ya.ru']},
        ],
        'dns': {'listen': ['198.18.0.53'], 'remote': ['1.1.1.1'], 'direct': ['8.8.8.8'],
                'hosts': {'proxy.routing.test': ['10.77.0.1']}},
    }
    for enabled in (False, True):
        connect(synthetic, enabled)
        if route_interface('1.1.1.1') == uplink:
            raise RuntimeError('The test did not install a full-tunnel route')
        if route_interface('1.1.1.1', uplink) != uplink:
            raise RuntimeError('Bound-interface routing still has no scoped default')
        proxy_address = dns('proxy.routing.test')
        if proxy_address != '10.77.0.1':
            raise RuntimeError('The router did not resolve the controlled proxy name')
        source = curl('http://proxy.routing.test:18080/', '--resolve',
                      'proxy.routing.test:18080:10.77.0.1')
        if source != credentials['deviceIpv4Address']:
            raise RuntimeError('The server did not observe the tunneled client address')
        print('PASS: encrypted AWG tunnel, server observed the client tunnel address', flush=True)
        direct_addresses = dns('api.ipify.org').splitlines()
        direct_address = next((answer for answer in direct_addresses
                               if re.fullmatch(r'\d+(?:\.\d+){3}', answer)), None)
        if not direct_address:
            raise RuntimeError('The router did not resolve the direct IP control endpoint')
        direct_ip = curl('https://api.ipify.org', '--resolve', f'api.ipify.org:443:{direct_address}')
        if direct_ip != baseline_ip:
            raise RuntimeError('A direct rule used the VPN server address')
        print('PASS: direct TCP used the physical public address', flush=True)
        blocked = run('sudo', '-u', run_as, 'dig', '+time=5', '+tries=1', '@198.18.0.53',
                      'example.com', 'A').stdout
        if 'status: NXDOMAIN' not in blocked:
            raise RuntimeError('Blocked DNS name was not rejected')
        print('PASS: block DNS returns NXDOMAIN', flush=True)
        for host in ('ya.ru',):
            address = dns(host).splitlines()[-1]
            direct_https(host, address)
        daemon.disconnect()
    connect(original_profile, True)
    for host in ('ya.ru', 'yandex.ru'):
        answers = dns(host).splitlines()
        addresses = [answer for answer in answers if re.fullmatch(r'\d+(?:\.\d+){3}', answer)]
        if not addresses or addresses[-1] == '0.0.0.0':
            raise RuntimeError(f'Original Happ profile did not resolve {host}')
        direct_https(host, addresses[-1])
    if dns('ya.ru', 'AAAA'):
        raise RuntimeError('Kill switch profile returned an unusable IPv6 answer')
    print('PASS: original Happ profile suppresses blocked IPv6 answers', flush=True)
    print(run('pfctl', '-a', 'amn-test/*', '-v', '-s', 'rules').stdout, flush=True)
finally:
    signal.alarm(0)
    original_error = sys.exc_info()[1]
    try:
        daemon.disconnect()
        print('PASS: daemon disconnect restored scoped-route ownership', flush=True)
    except (OSError, RuntimeError) as cleanup_error:
        if original_error is None:
            raise
        print(f'Cleanup also failed: {cleanup_error}', file=sys.stderr, flush=True)
    finally:
        daemon.socket.close()
