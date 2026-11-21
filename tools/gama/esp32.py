"""gama esp32: the ESP32 ground station on this machine's USB.

    gama esp32 port     which serial port it is on, or why there is none
                        (a charge-only cable, a missing driver: 20/11)
    gama esp32 flash    make esp32-flash on that port
"""

import subprocess

import common as c


def cmd_port(args):
    port, problem = c.esp32_port(args.port)
    if not port:
        c.fail('ESP32: ' + problem)
        return 1
    c.ok(f'ESP32 em {port}')
    return 0


def cmd_flash(args, call=subprocess.call):
    port, problem = c.esp32_port(args.port)
    if not port:
        c.fail('ESP32: ' + problem)
        return 1
    c.say(f'gravando o firmware da ground na ESP32 em {port} (ESP-IDF no Docker)')
    try:
        return call(['make', '-C', str(c.ROOT), 'esp32-flash', f'ESP32_PORT={port}'])
    except KeyboardInterrupt:
        return 130
