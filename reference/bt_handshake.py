#!/usr/bin/env python3
"""
Bluetooth RFCOMM Handshake for Android Auto Wireless Setup.

This script implements the head unit side of the Bluetooth handshake protocol
used by Android Auto for WiFi credential exchange. It registers the required
Bluetooth profiles (HFP AG, HSP HS, AA RFCOMM) via BlueZ ProfileManager1
and performs the 5-stage RFCOMM handshake.

Protocol (msgId values from BtProtocol.java):
  1: WifiStartRequest (HU -> Phone)
  7: WifiStartResponse (Phone -> HU)
  2: WifiInfoRequest (Phone -> HU)
  3: WifiInfoResponse (HU -> Phone)
  6: WifiConnectStatus (Phone -> HU)

Packet format: [length:u16_be][msg_id:u16_be][protobuf_payload]
"""

import argparse
import dbus
import dbus.service
import dbus.mainloop.glib
import socket
import struct
import sys
import logging
from gi.repository import GLib

# Configure logging
logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    datefmt='%H:%M:%S'
)
log = logging.getLogger(__name__)

# BlueZ D-Bus constants
BUS_NAME = "org.bluez"
PROFILE_MANAGER_IFACE = "org.bluez.ProfileManager1"
PROFILE_IFACE = "org.bluez.Profile1"

# UUIDs (from BtProtocol.java)
HFP_AG_UUID = "0000111f-0000-1000-8000-00805f9b34fb"
HSP_HS_UUID = "00001108-0000-1000-8000-00805f9b34fb"
AA_RFCOMM_UUID = "4de17a00-52cb-11e6-bdf4-0800200c9a66"

# RFCOMM channels
AA_CHANNEL = 8
HFP_CHANNEL = 15
HSP_CHANNEL = 1

# Message IDs (from BtProtocol.java)
WIFI_START_REQUEST = 1
WIFI_START_RESPONSE = 7
WIFI_INFO_REQUEST = 2
WIFI_INFO_RESPONSE = 3
WIFI_CONNECT_STATUS = 6

# WiFi Security Modes (from WifiSecurityModeEnum.proto)
SECURITY_MODE_WPA2_PERSONAL = 8

# WiFi Access Point Types (from WifiAccessPointTypeEnum.proto)
# Note: AP_TYPE_STATIC = 1 per the proto, but docs say DYNAMIC = 1
# Java code uses ApType = 1 which is STATIC in the proto
ACCESS_POINT_TYPE_STATIC = 1


# Protobuf encoding helpers
def encode_varint(value: int) -> bytes:
    """Encode unsigned integer as protobuf varint."""
    result = []
    while value >= 0x80:
        result.append((value & 0x7F) | 0x80)
        value >>= 7
    result.append(value & 0x7F)
    return bytes(result)


def encode_string(field_number: int, value: str) -> bytes:
    """Encode string field (wire type 2 = length-delimited)."""
    data = value.encode('utf-8')
    tag = (field_number << 3) | 2
    return encode_varint(tag) + encode_varint(len(data)) + data


def encode_uint32(field_number: int, value: int) -> bytes:
    """Encode uint32 field (wire type 0 = varint)."""
    tag = (field_number << 3) | 0
    return encode_varint(tag) + encode_varint(value)


def encode_int32(field_number: int, value: int) -> bytes:
    """Encode int32 field (wire type 0 = varint, zigzag for negative)."""
    # Zigzag encoding for signed integers
    zigzag = (value << 1) ^ (value >> 31)
    tag = (field_number << 3) | 0
    return encode_varint(tag) + encode_varint(zigzag)


def build_wifi_start_request(ip: str, port: int) -> bytes:
    """Build WifiStartRequest message (msgId=1)."""
    # Fields: ip_address=1 (string), port=2 (uint32)
    payload = encode_string(1, ip) + encode_uint32(2, port)
    length = len(payload)
    return struct.pack('>HH', length, WIFI_START_REQUEST) + payload


def build_wifi_info_response(ssid: str, key: str, bssid: str) -> bytes:
    """Build WifiInfoResponse message (msgId=3)."""
    # Wire format per docs/wireless-bluetooth-setup.md and Java implementation:
    # ssid=1 (string), key/passphrase=2 (string), bssid=3 (string)
    # security_mode=4 (enum), ap_type=5 (enum)
    payload = (
        encode_string(1, ssid) +
        encode_string(2, key) +
        encode_string(3, bssid) +
        encode_varint((4 << 3) | 0) + encode_varint(SECURITY_MODE_WPA2_PERSONAL) +
        encode_varint((5 << 3) | 0) + encode_varint(ACCESS_POINT_TYPE_STATIC)
    )
    length = len(payload)
    return struct.pack('>HH', length, WIFI_INFO_RESPONSE) + payload


def build_wifi_start_response(status: int = 0, ip: str = "", port: int = 0) -> bytes:
    """Build WifiStartResponse message (msgId=7) - for completeness."""
    payload = b''
    if ip:
        payload += encode_string(1, ip)
    if port:
        payload += encode_uint32(2, port)
    if status != 0:
        payload += encode_int32(3, status)
    length = len(payload)
    return struct.pack('>HH', length, WIFI_START_RESPONSE) + payload


def parse_message(payload: bytes, msg_id: int) -> dict:
    """Parse protobuf payload based on message type."""
    result = {'msg_id': msg_id, 'raw': payload}
    offset = 0

    def read_varint(data, off):
        val = 0
        shift = 0
        while True:
            b = data[off]
            off += 1
            val |= (b & 0x7F) << shift
            if not (b & 0x80):
                break
            shift += 7
        return val, off

    def read_string(data, off):
        length, off = read_varint(data, off)
        val = data[off:off+length].decode('utf-8')
        off += length
        return val, off

    while offset < len(payload):
        tag, offset = read_varint(payload, offset)
        field_num = tag >> 3
        wire_type = tag & 0x7

        if wire_type == 0:  # varint
            val, offset = read_varint(payload, offset)
            result[f'field_{field_num}'] = val
        elif wire_type == 2:  # length-delimited
            val, offset = read_string(payload, offset)
            result[f'field_{field_num}'] = val
        else:
            log.warning(f"Unknown wire type {wire_type} for field {field_num}")
            break

    return result


class BluetoothProfile(dbus.service.Object):
    """Base class for Bluetooth profiles."""

    def __init__(self, bus, path, uuid, channel, handler):
        self.path = path
        self.uuid = uuid
        self.channel = channel
        self.handler = handler
        dbus.service.Object.__init__(self, bus, path)

    @dbus.service.method(PROFILE_IFACE, in_signature='oha{sv}', out_signature='')
    def NewConnection(self, device, fd, fd_properties):
        """Called when a new Bluetooth connection is established."""
        try:
            sock = socket.fromfd(fd.take(), socket.AF_BLUETOOTH, socket.SOCK_STREAM)
            sock.setblocking(True)
            sock.settimeout(10.0)
            log.info(f"New connection from {device} on {self.uuid} (channel {self.channel})")
            self.handler(device, sock)
        except Exception as e:
            log.error(f"Error handling connection: {e}")
        finally:
            try:
                sock.close()
            except:
                pass

    @dbus.service.method(PROFILE_IFACE, in_signature='o', out_signature='')
    def RequestDisconnection(self, device):
        log.info(f"Disconnection requested for {device}")


class HFPProfile(BluetoothProfile):
    """Hands-Free Profile Audio Gateway - required by Android Auto."""

    def __init__(self, bus, path):
        super().__init__(bus, path, HFP_AG_UUID, HFP_CHANNEL, self.handle_connection)

    def handle_connection(self, device, sock):
        log.info(f"HFP AG connection from {device}")
        try:
            while True:
                data = sock.recv(1024)
                if not data:
                    break
                # Optional: respond to AT commands
                if b'AT' in data:
                    sock.sendall(b'OK\r\n')
        except socket.timeout:
            pass
        except Exception as e:
            log.debug(f"HFP connection ended: {e}")
        finally:
            log.info(f"HFP AG disconnected: {device}")


class HSPProfile(BluetoothProfile):
    """Headset Profile Headset - keeps BT connection alive."""

    def __init__(self, bus, path):
        super().__init__(bus, path, HSP_HS_UUID, HSP_CHANNEL, self.handle_connection)

    def handle_connection(self, device, sock):
        log.info(f"HSP HS connection from {device}")
        try:
            while True:
                data = sock.recv(1024)
                if not data:
                    break
        except socket.timeout:
            pass
        except Exception as e:
            log.debug(f"HSP connection ended: {e}")
        finally:
            log.info(f"HSP HS disconnected: {device}")


class AAProfile(BluetoothProfile):
    """Android Auto RFCOMM Profile - handles WiFi credential handshake."""

    def __init__(self, bus, path, ssid, key, bssid, ip, port):
        super().__init__(bus, path, AA_RFCOMM_UUID, AA_CHANNEL, self.handle_connection)
        self.ssid = ssid
        self.key = key
        self.bssid = bssid
        self.ip = ip
        self.port = port

    def handle_connection(self, device, sock):
        log.info(f"AA RFCOMM connection from {device}")
        try:
            # Stage 1: Send WifiStartRequest (msgId=1)
            start_req = build_wifi_start_request(self.ip, self.port)
            sock.sendall(start_req)
            log.info(f"→ Sent WifiStartRequest (msgId={WIFI_START_REQUEST})")
            log.debug(f"  IP={self.ip}, Port={self.port}")

            while True:
                # Read header (4 bytes: length u16_be + msg_id u16_be)
                header = self._read_exact(sock, 4)
                if not header:
                    log.info("Connection closed by peer")
                    break

                length, msg_id = struct.unpack('>HH', header)
                log.debug(f"← Received header: length={length}, msgId={msg_id}")

                # Read payload
                payload = self._read_exact(sock, length)
                if payload is None:
                    break

                parsed = parse_message(payload, msg_id)
                log.debug(f"  Parsed: {parsed}")

                # Stage 2: Handle WifiStartResponse (msgId=7)
                if msg_id == WIFI_START_RESPONSE:
                    status = parsed.get('field_3', 0)
                    log.info(f"← Received WifiStartResponse (msgId={WIFI_START_RESPONSE}): status={status}")
                    if status == 0:
                        # Stage 3: Phone will send WifiInfoRequest next
                        pass
                    else:
                        log.warning(f"WifiStartRequest failed with status {status}")
                        break

                # Stage 3: Handle WifiInfoRequest (msgId=2)
                elif msg_id == WIFI_INFO_REQUEST:
                    log.info(f"← Received WifiInfoRequest (msgId={WIFI_INFO_REQUEST})")

                    # Stage 4: Send WifiInfoResponse (msgId=3)
                    info_resp = build_wifi_info_response(self.ssid, self.key, self.bssid)
                    sock.sendall(info_resp)
                    log.info(f"→ Sent WifiInfoResponse (msgId={WIFI_INFO_RESPONSE})")
                    log.debug(f"  SSID={self.ssid}, BSSID={self.bssid}")

                # Stage 5: Handle WifiConnectStatus (msgId=6)
                elif msg_id == WIFI_CONNECT_STATUS:
                    status = parsed.get('field_1', -1)
                    status_text = parsed.get('field_2', '')
                    log.info(f"← Received WifiConnectStatus (msgId={WIFI_CONNECT_STATUS}): status={status}")
                    if status_text:
                        log.info(f"  Status text: {status_text}")
                    if status == 0:
                        log.info("✓ WiFi setup complete! Phone should now connect via TCP")
                    else:
                        log.warning(f"WiFi connection failed with status {status}")
                    break

                else:
                    log.warning(f"Unexpected message ID: {msg_id}")

        except socket.timeout:
            log.warning("AA RFCOMM timeout")
        except Exception as e:
            log.error(f"AA handshake error: {e}")
        finally:
            log.info(f"AA RFCOMM disconnected: {device}")

    def _read_exact(self, sock, n):
        """Read exactly n bytes from socket."""
        data = b''
        while len(data) < n:
            try:
                chunk = sock.recv(n - len(data))
                if not chunk:
                    return None
                data += chunk
            except socket.timeout:
                return None
            except Exception as e:
                log.error(f"Read error: {e}")
                return None
        return data


def register_profile(bus, manager, profile, path, uuid, channel, name):
    """Register a Bluetooth profile with BlueZ."""
    opts = {
        "Channel": dbus.UInt16(channel),
        "RequireAuthentication": dbus.Boolean(False),
        "RequireAuthorization": dbus.Boolean(False),
        "AutoConnect": dbus.Boolean(True),
        "Name": name,
        "Role": "server",
    }
    try:
        manager.RegisterProfile(dbus.ObjectPath(path), uuid, opts)
        log.info(f"Registered profile {name} ({uuid}) on channel {channel}")
        return True
    except dbus.DBusException as e:
        log.error(f"Failed to register {name} ({uuid}): {e}")
        return False


def unregister_profile(bus, path):
    """Unregister a Bluetooth profile."""
    try:
        bus.call_blocking(
            BUS_NAME, "/org/bluez", PROFILE_MANAGER_IFACE, "UnregisterProfile",
            "o", dbus.ObjectPath(path)
        )
        log.info(f"Unregistered profile at {path}")
    except Exception as e:
        log.debug(f"Unregister failed for {path}: {e}")


def main():
    parser = argparse.ArgumentParser(description="Android Auto Bluetooth Handshake Server")
    parser.add_argument("--ssid", default="AndroidAuto", help="WiFi SSID")
    parser.add_argument("--key", default="password123", help="WiFi password/key")
    parser.add_argument("--bssid", default="AA:BB:CC:DD:EE:FF", help="WiFi BSSID (MAC address)")
    parser.add_argument("--ip", default="10.0.0.1", help="Head unit IP address for TCP")
    parser.add_argument("--port", type=int, default=5288, help="Head unit TCP port")
    parser.add_argument("--debug", action="store_true", help="Enable debug logging")
    args = parser.parse_args()

    if args.debug:
        log.setLevel(logging.DEBUG)

    log.info("Starting Android Auto Bluetooth Handshake Server")
    log.info(f"WiFi: SSID={args.ssid}, BSSID={args.bssid}, Key={args.key}")
    log.info(f"TCP: {args.ip}:{args.port}")

    # Initialize D-Bus main loop
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    # Get ProfileManager1 interface
    try:
        manager_obj = bus.get_object(BUS_NAME, "/org/bluez")
        manager = dbus.Interface(manager_obj, PROFILE_MANAGER_IFACE)
    except dbus.DBusException as e:
        log.error(f"Failed to connect to BlueZ: {e}")
        log.error("Make sure BlueZ is running with --compat flag")
        sys.exit(1)

    # Create profile instances
    hfp = HFPProfile(bus, "/org/bluez/hfp_ag")
    hsp = HSPProfile(bus, "/org/bluez/hsp_hs")
    aa = AAProfile(bus, "/org/bluez/aa_rfcomm", args.ssid, args.key, args.bssid, args.ip, args.port)

    profiles = [
        (hfp, HFP_AG_UUID, HFP_CHANNEL, "/org/bluez/hfp_ag", "HFP AG"),
        (hsp, HSP_HS_UUID, HSP_CHANNEL, "/org/bluez/hsp_hs", "HSP HS"),
        (aa, AA_RFCOMM_UUID, AA_CHANNEL, "/org/bluez/aa_rfcomm", "AA RFCOMM"),
    ]

    # Register all profiles
    for profile, uuid, channel, path, name in profiles:
        if not register_profile(bus, manager, profile, path, uuid, channel, name):
            # Cleanup on failure
            for _, _, _, p, _ in profiles:
                unregister_profile(bus, p)
            sys.exit(1)

    log.info("All profiles registered successfully")
    log.info("Waiting for Android Auto connection...")
    log.info("Press Ctrl+C to stop")

    # Run main loop
    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        log.info("\nShutting down...")
    finally:
        for _, _, _, path, _ in profiles:
            unregister_profile(bus, path)


if __name__ == "__main__":
    main()