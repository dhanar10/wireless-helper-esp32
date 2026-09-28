#!/usr/bin/env python3

import dbus
import dbus.service
import dbus.mainloop.glib
import sys
from gi.repository import GLib

BUS_NAME = "org.bluez"
AGENT_MANAGER_IFACE = "org.bluez.AgentManager1"
AGENT_IFACE = "org.bluez.Agent1"

AGENT_PATH = "/org/bluez/bt_agent"
CAPABILITY = "DisplayYesNo"


class BTAgent(dbus.service.Object):
    def __init__(self, bus, path):
        self.path = path
        dbus.service.Object.__init__(self, bus, path)

    @dbus.service.method(AGENT_IFACE, in_signature='', out_signature='')
    def Release(self):
        print("Agent released")

    @dbus.service.method(AGENT_IFACE, in_signature='os', out_signature='')
    def RequestPinCode(self, device, pin):
        print(f"RequestPinCode({device}, {pin})")
        raise dbus.DBusException("org.bluez.Error.Rejected", "Not supported")

    @dbus.service.method(AGENT_IFACE, in_signature='ou', out_signature='')
    def DisplayPinCode(self, device, pincode):
        print(f"DisplayPinCode({device}, {pincode:06d})")

    @dbus.service.method(AGENT_IFACE, in_signature='ou', out_signature='')
    def RequestPasskey(self, device, passkey):
        print(f"RequestPasskey({device})")
        raise dbus.DBusException("org.bluez.Error.Rejected", "Not supported")

    @dbus.service.method(AGENT_IFACE, in_signature='ouq', out_signature='')
    def DisplayPasskey(self, device, passkey, entered):
        print(f"DisplayPasskey({device}, {passkey:06d}, entered={entered})")

    @dbus.service.method(AGENT_IFACE, in_signature='ou', out_signature='')
    def RequestConfirmation(self, device, passkey):
        print(f"RequestConfirmation({device}, {passkey:06d}) - auto confirming")
        return

    @dbus.service.method(AGENT_IFACE, in_signature='os', out_signature='')
    def RequestAuthorization(self, device, uuid):
        print(f"RequestAuthorization({device}, {uuid}) - auto authorizing")
        return

    @dbus.service.method(AGENT_IFACE, in_signature='s', out_signature='')
    def AuthorizeService(self, uuid):
        print(f"AuthorizeService({uuid}) - auto authorizing")
        return

    @dbus.service.method(AGENT_IFACE, in_signature='', out_signature='')
    def Cancel(self):
        print("Agent cancelled")


def main():
    dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    manager_obj = bus.get_object(BUS_NAME, "/org/bluez")
    manager = dbus.Interface(manager_obj, AGENT_MANAGER_IFACE)

    agent = BTAgent(bus, AGENT_PATH)

    try:
        manager.RegisterAgent(AGENT_PATH, CAPABILITY)
        print(f"Agent registered with capability: {CAPABILITY}")

        manager.RequestDefaultAgent(AGENT_PATH)
        print("Default agent requested")

    except dbus.DBusException as e:
        print(f"Failed to register agent: {e}")
        sys.exit(1)

    print("Agent running. Press Ctrl+C to exit.")
    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        print("\nShutting down...")
    finally:
        try:
            manager.UnregisterAgent(AGENT_PATH)
        except:
            pass


if __name__ == "__main__":
    main()