# ------------------------------------------------------------------------------
# Copyright © m2ag.labs (marc@m2ag.net). All rights reserved.
# Licensed under the MIT license.
# ------------------------------------------------------------------------------

import PyCmdMessenger
import serial
import time
import json

# usually /dev/ttyACM0 on raspberry pi.
COMM_PORT = "/dev/cu.usbmodem14501"
BAUD_RATE = 57600

# Application to test m2ag.labs aqsensor
# Adjust comm port and baud rate accordingly.
# Offsets will trim sensor output. Changes will be stored in
# device flash, so flash write cautions apply.
# use a string --
offsets = "t-1.4b0000p0000h000000e000000"
# where t = temperature, b = barometric pressure, p is pm25, h is humidity
# and e is eCo2. Be sure to match the lengths. BME280 temps are always too high
# due to the sensor heating up.

# List of commands and their associated argument formats. These must be in the
# same order as in the sketch.
commands = [["poll_all", ""],
            ["report_all", "s*"],
            ["set_offsets", "s*"],
            ["error", "s"]]


# Initialize the messenger
def connect():
    try:
        arduino = PyCmdMessenger.ArduinoBoard(COMM_PORT, baud_rate=BAUD_RATE)
        _c = PyCmdMessenger.CmdMessenger(arduino, commands)
        time.sleep(2)
        return _c
    except serial.serialutil.SerialException:
        print("connect error")


c = connect()

# Send
while True:
    try:
        c.send("poll_all")
        # Receive. Should give ["report_all", data_string ,TIME_RECIEVED]
        msg = c.receive()
        string = ""

        for ele in msg[1]:
            string += ele
            if msg[1][len(msg[1]) - 1] != ele:
                string += ","

        print(json.loads(string))

        # uncomment to send offsets. Device only updates if
        # a change is sent. After every device flash the offsets need to be
        # resent.
        # c.send('set_offsets', *offsets)
    except:
        c = connect()

    # print(msg)
    time.sleep(1)
