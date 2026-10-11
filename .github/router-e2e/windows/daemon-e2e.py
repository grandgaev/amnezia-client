#!/usr/bin/env python3
import argparse
import ctypes
from ctypes import wintypes
import ipaddress
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import time


PIPE_NAME = r"\\.\pipe\amneziavpn"
INVALID_HANDLE_VALUE = wintypes.HANDLE(-1).value


def run(*args, check=True, timeout=30):
    return subprocess.run(args, capture_output=True, text=True, check=check,
                          timeout=timeout)


def curl(url, *options):
    return curl_with('curl.exe', url, *options)


def curl_with(executable, url, *options):
    return run(str(executable), '-4', '-fsS', '--connect-timeout', '10',
               '--max-time', '20', '--noproxy', '*', *options, url).stdout.strip()


def powershell(script, check=True):
    return run('powershell.exe', '-NoLogo', '-NoProfile', '-NonInteractive',
               '-Command', script, check=check).stdout.strip()


def route_interface_index(address):
    output = powershell(
        f"(Find-NetRoute -RemoteIPAddress '{address}' -ErrorAction Stop).InterfaceIndex")
    indexes = [line.strip() for line in output.splitlines() if line.strip().isdigit()]
    if not indexes:
        raise RuntimeError(f'Windows did not return a route for {address}')
    return int(indexes[0])


def management_addresses():
    addresses = set()
    output = powershell(
        "Get-NetTCPConnection -State Established -ErrorAction SilentlyContinue | "
        "Select-Object -ExpandProperty RemoteAddress", check=False)
    for candidate in output.splitlines():
        try:
            address = ipaddress.ip_address(candidate.strip())
            if address.version == 4 and not address.is_loopback:
                addresses.add(str(address))
        except ValueError:
            pass
    for domain in ('api.github.com', 'github.com', 'objects.githubusercontent.com',
                   'results-receiver.actions.githubusercontent.com'):
        try:
            addresses.update(info[4][0] for info in
                             socket.getaddrinfo(domain, 443, socket.AF_INET))
        except OSError:
            pass
    return sorted(addresses)


def encode_dns_name(name):
    return b''.join(bytes((len(label),)) + label.encode('ascii')
                    for label in name.rstrip('.').split('.')) + b'\0'


def skip_dns_name(packet, offset):
    while True:
        length = packet[offset]
        if length & 0xc0 == 0xc0:
            return offset + 2
        offset += 1
        if length == 0:
            return offset
        offset += length


def dns_query(name, expected_rcode=0):
    query_id = int.from_bytes(os.urandom(2), 'big')
    query = struct.pack('!HHHHHH', query_id, 0x0100, 1, 0, 0, 0)
    query += encode_dns_name(name) + struct.pack('!HH', 1, 1)
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as dns_socket:
        dns_socket.settimeout(10)
        dns_socket.sendto(query, ('198.18.0.53', 53))
        packet, _ = dns_socket.recvfrom(4096)
    if len(packet) < 12:
        raise RuntimeError(f'Truncated DNS response for {name}')
    response_id, flags, qdcount, ancount, _, _ = struct.unpack('!HHHHHH', packet[:12])
    if response_id != query_id or not flags & 0x8000:
        raise RuntimeError(f'Invalid DNS response for {name}')
    rcode = flags & 0x0f
    if rcode != expected_rcode:
        raise RuntimeError(
            f'DNS response for {name} has rcode {rcode}, expected {expected_rcode}')
    offset = 12
    for _ in range(qdcount):
        offset = skip_dns_name(packet, offset) + 4
    answers = []
    for _ in range(ancount):
        offset = skip_dns_name(packet, offset)
        record_type, record_class, _, length = struct.unpack(
            '!HHIH', packet[offset:offset + 10])
        offset += 10
        data = packet[offset:offset + length]
        offset += length
        if record_type == 1 and record_class == 1 and length == 4:
            answers.append(socket.inet_ntoa(data))
    return answers


def assert_kill_switch_filters():
    output = Path(os.environ['RUNNER_TEMP']) / 'amnezia-wfp-filters.xml'
    try:
        run('netsh.exe', 'wfp', 'show', 'filters', f'file={output}',
            timeout=45)
        raw = output.read_bytes()
        if raw.startswith((b'\xff\xfe', b'\xfe\xff')) or b'\0' in raw[:100]:
            state = raw.decode('utf-16', errors='replace')
        else:
            state = raw.decode('utf-8-sig', errors='replace')
        required = ('Block Internet', 'Block all DNS',
                    'Allow usage of VPN Adapter')
        missing = [name for name in required if name not in state]
        if missing:
            raise RuntimeError(
                f'Kill switch WFP filters are missing: {", ".join(missing)}')
    finally:
        output.unlink(missing_ok=True)


class NamedPipe:
    def __init__(self, timeout=45):
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel32 = kernel32
        kernel32.WaitNamedPipeW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD]
        kernel32.WaitNamedPipeW.restype = wintypes.BOOL
        kernel32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD,
                                         wintypes.DWORD, wintypes.LPVOID,
                                         wintypes.DWORD, wintypes.DWORD,
                                         wintypes.HANDLE]
        kernel32.CreateFileW.restype = wintypes.HANDLE
        kernel32.WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPCVOID,
                                       wintypes.DWORD,
                                       ctypes.POINTER(wintypes.DWORD),
                                       wintypes.LPVOID]
        kernel32.WriteFile.restype = wintypes.BOOL
        kernel32.ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID,
                                      wintypes.DWORD,
                                      ctypes.POINTER(wintypes.DWORD),
                                      wintypes.LPVOID]
        kernel32.ReadFile.restype = wintypes.BOOL
        kernel32.PeekNamedPipe.argtypes = [wintypes.HANDLE, wintypes.LPVOID,
                                           wintypes.DWORD,
                                           ctypes.POINTER(wintypes.DWORD),
                                           ctypes.POINTER(wintypes.DWORD),
                                           ctypes.POINTER(wintypes.DWORD)]
        kernel32.PeekNamedPipe.restype = wintypes.BOOL
        kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel32.CloseHandle.restype = wintypes.BOOL

        if not kernel32.WaitNamedPipeW(PIPE_NAME, int(timeout * 1000)):
            raise ctypes.WinError(ctypes.get_last_error())
        self.handle = kernel32.CreateFileW(
            PIPE_NAME, 0x80000000 | 0x40000000, 0, None, 3, 0, None)
        if self.handle == INVALID_HANDLE_VALUE:
            raise ctypes.WinError(ctypes.get_last_error())
        self.buffer = bytearray()

    def close(self):
        if self.handle != INVALID_HANDLE_VALUE:
            self.kernel32.CloseHandle(self.handle)
            self.handle = INVALID_HANDLE_VALUE

    def send(self, command):
        payload = json.dumps(command, separators=(',', ':')).encode() + b'\n'
        written = wintypes.DWORD()
        data = ctypes.create_string_buffer(payload)
        if not self.kernel32.WriteFile(self.handle, data, len(payload),
                                       ctypes.byref(written), None):
            raise ctypes.WinError(ctypes.get_last_error())
        if written.value != len(payload):
            raise RuntimeError('The daemon accepted only part of a command')

    def _read_available(self, deadline):
        while time.monotonic() < deadline:
            available = wintypes.DWORD()
            if not self.kernel32.PeekNamedPipe(
                    self.handle, None, 0, None, ctypes.byref(available), None):
                raise ctypes.WinError(ctypes.get_last_error())
            if available.value:
                chunk = ctypes.create_string_buffer(min(available.value, 4096))
                read = wintypes.DWORD()
                if not self.kernel32.ReadFile(self.handle, chunk, len(chunk),
                                              ctypes.byref(read), None):
                    raise ctypes.WinError(ctypes.get_last_error())
                self.buffer.extend(chunk.raw[:read.value])
                return
            time.sleep(0.05)
        raise TimeoutError('Timed out waiting for the daemon response')

    def wait(self, kind, timeout=60):
        deadline = time.monotonic() + timeout
        while True:
            newline = self.buffer.find(b'\n')
            if newline < 0:
                self._read_available(deadline)
                continue
            line = bytes(self.buffer[:newline]).strip()
            del self.buffer[:newline + 1]
            if not line:
                continue
            message = json.loads(line)
            if message.get('type') == 'backendFailure':
                raise RuntimeError(
                    f"Daemon backend failed with code {message.get('errorCode')}")
            if message.get('type') == 'disconnected' and kind != 'disconnected':
                raise RuntimeError('Daemon rejected the test connection')
            if message.get('type') == kind:
                return message

    def disconnect(self, uplink=None):
        self.send({'type': 'deactivate'})
        self.wait('disconnected', timeout=30)
        self.send({'type': 'status'})
        if self.wait('status', timeout=10).get('connected'):
            raise RuntimeError('Daemon still reports a connected tunnel')
        if uplink is not None:
            deadline = time.monotonic() + 10
            while True:
                try:
                    if route_interface_index('1.1.1.1') == uplink:
                        break
                except (RuntimeError, subprocess.CalledProcessError):
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError(
                        'The physical route was not restored after disconnect')
                time.sleep(0.2)


def require_disposable_runner():
    if sys.platform != 'win32' or os.environ.get('GITHUB_ACTIONS') != 'true':
        sys.exit('This test changes Windows routes and requires a GitHub runner')
    shell32 = ctypes.WinDLL('shell32', use_last_error=True)
    if not shell32.IsUserAnAdmin():
        sys.exit('The daemon acceptance test must run as an administrator')


def cleanup_only():
    require_disposable_runner()
    pipe = NamedPipe(timeout=5)
    try:
        pipe.disconnect()
    finally:
        pipe.close()


def connect(pipe, credentials, routing, kill_switch, management, uplink,
            vpn_disabled_apps=None):
    vpn_disabled_apps = vpn_disabled_apps or []
    config = dict(credentials)
    config.update({
        'type': 'activate',
        'deviceIpv4Address': str(
            ipaddress.ip_interface(credentials['deviceIpv4Address'] + '/32')),
        'primaryDnsServer': '198.18.0.53',
        'secondaryDnsServer': '198.18.0.53',
        'allowedIPAddressRanges': [
            {'address': '0.0.0.0', 'range': 0, 'isIpv6': False}],
        'excludedAddresses': sorted({credentials['serverIpv4AddrIn'], *management}),
        'vpnDisabledApps': vpn_disabled_apps,
        'allowedDnsServers': [],
        'killSwitchOption': 'true' if kill_switch else 'false',
        'routingConfig': routing,
    })
    pipe.send(config)
    pipe.wait('connected', timeout=90)
    pipe.send({'type': 'status'})
    if not pipe.wait('status', timeout=10).get('connected'):
        raise RuntimeError('Daemon did not confirm the live tunnel')
    for address in {credentials['serverIpv4AddrIn'], *management}:
        if route_interface_index(address) != uplink:
            raise RuntimeError(
                'A server or runner-management exclusion left the uplink')
    print(f'PASS: daemon connected, kill switch {kill_switch}', flush=True)


def main(config_path):
    require_disposable_runner()
    credentials = json.loads(Path(config_path).read_text(encoding='utf-8-sig'))
    required_fields = ('privateKey', 'serverPublicKey', 'serverIpv4AddrIn',
                       'serverIpv4Gateway', 'deviceIpv4Address')
    if any(not isinstance(credentials.get(field), str) or not credentials[field]
           for field in required_fields):
        sys.exit('The isolated server configuration is missing required fields')
    if not isinstance(credentials.get('serverPort'), int):
        sys.exit('The isolated server port must be a number')
    credentials['deviceIpv4Address'] = str(
        ipaddress.IPv4Interface(credentials['deviceIpv4Address']).ip)
    for field in ('serverIpv4AddrIn', 'serverIpv4Gateway'):
        credentials[field] = str(ipaddress.IPv4Address(credentials[field]))
    if (credentials['deviceIpv4Address'] != '10.77.0.3'
            or not ipaddress.ip_address(credentials['serverIpv4AddrIn']).is_global
            or not 0 < credentials['serverPort'] < 65536):
        sys.exit('The Windows secret does not describe the isolated client3 lab peer')

    management = management_addresses()
    uplink = route_interface_index(credentials['serverIpv4AddrIn'])
    baseline_ip = curl('https://api.ipify.org')
    public_addresses = sorted({info[4][0] for info in
                               socket.getaddrinfo('api.ipify.org', 443, socket.AF_INET)})
    if not public_addresses:
        sys.exit('The direct control endpoint did not resolve before connecting')
    if not socket.gethostbyname_ex('example.com')[2]:
        sys.exit('The block control name did not resolve before connecting')
    print(f'Baseline uplink index {uplink}; management exclusions {len(management)}',
          flush=True)

    routing = {
        'version': 1,
        'defaultAction': 'proxy',
        'bypass': 'interface',
        'rejectDirectIpv6': True,
        'rejectProxyIpv6': True,
        'rules': [
            {'action': 'block', 'domains': ['full:example.com']},
            {'action': 'direct', 'domains': ['full:api.ipify.org']},
        ],
        'dns': {
            'listen': ['198.18.0.53'],
            'remote': ['1.1.1.1'],
            'direct': ['8.8.8.8'],
            'hosts': {'proxy.routing.test': ['10.77.0.1']},
        },
    }

    pipe = NamedPipe()
    copied_probes = []
    original_error = None
    try:
        for kill_switch in (False, True):
            connect(pipe, credentials, routing, kill_switch, management, uplink)
            if kill_switch:
                assert_kill_switch_filters()
                print('PASS: kill switch WFP filters are active', flush=True)
            if route_interface_index('1.1.1.1') == uplink:
                raise RuntimeError('The daemon did not install a full-tunnel route')
            proxy_answers = dns_query('proxy.routing.test')
            if proxy_answers != ['10.77.0.1']:
                raise RuntimeError('The router did not resolve its controlled host')
            observed = curl('http://proxy.routing.test:18080/', '--resolve',
                            'proxy.routing.test:18080:10.77.0.1')
            if observed != credentials['deviceIpv4Address']:
                raise RuntimeError(
                    'The server did not observe the client tunnel address')
            print('PASS: encrypted AWG tunnel reached the isolated server', flush=True)

            direct_answers = dns_query('api.ipify.org')
            if not direct_answers:
                raise RuntimeError('The router returned no direct endpoint address')
            if route_interface_index(direct_answers[0]) == uplink:
                raise RuntimeError(
                    'The direct endpoint escaped through an OS host exclusion')
            direct_ip = curl('https://api.ipify.org', '--resolve',
                             f'api.ipify.org:443:{direct_answers[0]}')
            if direct_ip != baseline_ip:
                raise RuntimeError('A direct rule used the VPN server address')
            print('PASS: direct rule kept the physical public address', flush=True)

            dns_query('example.com', expected_rcode=3)
            print('PASS: block rule returned NXDOMAIN', flush=True)
            pipe.disconnect(uplink)

        system_curl = Path(shutil.which('curl.exe') or '')
        if not system_curl.is_file():
            raise RuntimeError('The Windows curl executable was not found')
        probe_dir = Path(os.environ['RUNNER_TEMP']) / 'amnezia-app-probes'
        probe_dir.mkdir(exist_ok=True)
        vpn_probe = probe_dir / 'vpn-probe.exe'
        direct_probe = probe_dir / 'direct-probe.exe'
        for probe in (vpn_probe, direct_probe):
            shutil.copy2(system_curl, probe)
            copied_probes.append(probe)
        app_routing = dict(routing)
        app_routing['rules'] = [
            {'action': 'block', 'domains': ['full:example.com']}]
        app_routing['apps'] = {
            'mode': 'exclude', 'paths': [str(direct_probe)]}
        connect(pipe, credentials, app_routing, False, management, uplink,
                [str(direct_probe)])
        control_address = public_addresses[0]
        direct_ip = curl_with(
            direct_probe, 'https://api.ipify.org', '--resolve',
            f'api.ipify.org:443:{control_address}')
        vpn_ip = curl_with(
            vpn_probe, 'https://api.ipify.org', '--resolve',
            f'api.ipify.org:443:{control_address}')
        if direct_ip != baseline_ip:
            raise RuntimeError('The excluded application did not bypass the VPN')
        if vpn_ip == baseline_ip:
            raise RuntimeError('The non-excluded application bypassed the VPN')
        print('PASS: per-app exclusion separates two executable paths', flush=True)
        pipe.disconnect(uplink)
    except BaseException as error:
        original_error = error
        raise
    finally:
        try:
            pipe.send({'type': 'status'})
            if pipe.wait('status', timeout=5).get('connected'):
                pipe.disconnect(uplink)
        except (OSError, RuntimeError, TimeoutError) as cleanup_error:
            if original_error is None:
                raise
            print(f'Cleanup also failed: {cleanup_error}', file=sys.stderr, flush=True)
        finally:
            pipe.close()
            for probe in copied_probes:
                probe.unlink(missing_ok=True)
            if copied_probes:
                try:
                    copied_probes[0].parent.rmdir()
                except OSError:
                    pass


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('config', nargs='?')
    parser.add_argument('--cleanup', action='store_true')
    args = parser.parse_args()
    if args.cleanup:
        cleanup_only()
    elif args.config:
        main(args.config)
    else:
        parser.error('a config path is required')
