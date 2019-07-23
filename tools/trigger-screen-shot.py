#!/usr/bin/python3

import dbus
import secrets
import re

from gi.repository import GLib
from dbus.mainloop.glib import DBusGMainLoop

class PortalBus:
    def __init__(self):
        DBusGMainLoop(set_as_default=True)

        self.bus = dbus.SessionBus()
        self.portal = self.bus.get_object('org.freedesktop.portal.Desktop', '/org/freedesktop/portal/desktop')

    def sender_name(self):
        return re.sub('\.', '_', self.bus.get_unique_name()).lstrip(':')

    def request_handle(self, token):
        return '/org/freedesktop/portal/desktop/request/%s/%s'%(self.sender_name(), token)

class PortalScreenshot:
    def __init__(self, portal_bus):
        self.portal_bus = portal_bus
        self.bus = portal_bus.bus
        self.portal = portal_bus.portal

    def request(self, callback, parent_window = ''):
        request_token = self.new_unique_token()
        options = { 'handle_token': request_token }

        self.bus.add_signal_receiver(callback,
                                    'Response',
                                    'org.freedesktop.portal.Request',
                                    'org.freedesktop.portal.Desktop',
                                    self.portal_bus.request_handle(request_token))

        self.portal.Screenshot(parent_window, options, dbus_interface='org.freedesktop.portal.Screenshot')

    @staticmethod
    def new_unique_token():
        return 'screen_shot_py_%s'%secrets.token_hex(16)

def callback(response, result):
    if response == 0:
        print(result['uri'])
    else:
        print("Failed to screenshot: %d"%response)

    loop.quit()

loop = GLib.MainLoop()
bus = PortalBus()
PortalScreenshot(bus).request(callback)

try:
    loop.run()
except KeyboardInterrupt:
    loop.quit()
