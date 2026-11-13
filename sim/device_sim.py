#!/usr/bin/env python3
"""Async TCP and PTY Modbus simulator, using only the standard library."""
import argparse
import asyncio
import errno
import os
import pty
import random
import struct
import tty


def crc(data):
    value = 0xffff
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xa001 if value & 1 else 0)
    return struct.pack('<H', value)


def response(pdu, registers):
    if not pdu:
        return b'\x80\x03'
    fc = pdu[0]
    def error(code):
        return bytes((fc | 0x80, code))
    if fc not in (3, 6, 16):
        return error(1)
    if len(pdu) < 5:
        return error(3)
    address, count = struct.unpack('>HH', pdu[1:5])
    if fc == 6:
        if len(pdu) != 5:
            return error(3)
        if address >= len(registers):
            return error(2)
        registers[address] = count
        return pdu
    if not 1 <= count <= (125 if fc == 3 else 123):
        return error(3)
    if address + count > len(registers):
        return error(2)
    if fc == 3:
        if len(pdu) != 5:
            return error(3)
        return bytes((fc, count * 2)) + struct.pack('>' + 'H' * count, *registers[address:address+count])
    if len(pdu) != 6 + count * 2 or pdu[5] != count * 2:
        return error(3)
    registers[address:address+count] = struct.unpack('>' + 'H' * count, pdu[6:])
    return pdu[:5]


async def pause(args, index):
    delay = max(0, args.latency_ms + random.uniform(-args.jitter_ms, args.jitter_ms))
    if index == args.fault_device and args.fault == 'delay':
        delay += args.fault_delay_ms
    await asyncio.sleep(delay / 1000)


async def tcp_client(reader, writer, registers, args, index):
    args.connections[index].add(writer)
    try:
        while True:
            header = await reader.readexactly(7)
            tx, proto, length, unit = struct.unpack('>HHHB', header)
            if proto or not 2 <= length <= 254:
                break
            pdu = await reader.readexactly(length - 1)
            if index == args.fault_device and args.fault == 'drop-connection':
                break
            if index in args.silenced:
                continue  # Deliberately discard replies; reconnects stay silent too.
            await args.gates[index].wait()
            fault = dict(args.device_faults[index])
            await pause(args, index)
            await asyncio.sleep(fault.get('delay', 0) / 1000)
            reply = response(pdu, registers)
            if 'exception' in fault:
                reply = bytes((pdu[0] | 0x80, fault['exception']))
            if fault.get('garbage'):
                reply = b'\x7f\x00'
            if fault.get('wrong-tx'):
                tx ^= 0xffff

            if index == args.fault_device and args.fault == 'garbage-response':
                reply = b'\x7f\x00'
            packet = struct.pack('>HHHB', tx, 0, len(reply)+1, unit) + reply
            if fault.get('partial'):
                writer.write(packet[:len(packet)//2])
                await writer.drain()
                await reader.read()  # Stall until peer disconnects; no polling.
                break
            writer.write(packet)
            await writer.drain()
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        args.connections[index].discard(writer)
        writer.close()
        try:
            await writer.wait_closed()
        except ConnectionError:
            pass


async def serial(args):
    master, slave = pty.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    print(os.ttyname(slave), flush=True)
    queue = asyncio.Queue()
    loop = asyncio.get_running_loop()
    def readable():
        try:
            data = os.read(master, 4096)
            if data:
                queue.put_nowait(data)
        except OSError as exc:
            if exc.errno not in (errno.EAGAIN, errno.EIO):
                raise
    loop.add_reader(master, readable)
    buffer = bytearray()
    registers = [0] * 1024
    try:
        while True:
            buffer.extend(await queue.get())
            while len(buffer) >= 2:
                fc = buffer[1]
                if fc == 16:
                    if len(buffer) < 7:
                        break
                    size = 9 + buffer[6]
                else:
                    size = 8
                if len(buffer) < size:
                    break
                packet = bytes(buffer[:size])
                del buffer[:size]
                if crc(packet[:-2]) != packet[-2:]:
                    continue
                if args.fault == 'drop-connection':
                    return
                await pause(args, 0)
                reply = packet[:1] + response(packet[1:-2], registers)
                reply += crc(reply)
                if args.fault == 'garbage-response':
                    reply = reply[:-1] + bytes((reply[-1] ^ 255,))
                view = memoryview(reply)
                while view:
                    try:
                        view = view[os.write(master, view):]
                    except BlockingIOError:
                        ready = loop.create_future()
                        loop.add_writer(master, lambda: ready.done() or ready.set_result(None))
                        try:
                            await ready
                        finally:
                            loop.remove_writer(master)
    finally:
        loop.remove_reader(master)
        os.close(master)
        os.close(slave)


async def main(args):
    if args.serial:
        await serial(args)
        return
    args.gates = [asyncio.Event() for _ in range(args.tcp_count)]
    for gate in args.gates:
        gate.set()
    servers = []
    if args.control_port:
        async def control(reader, writer):
            try:
                line = await reader.readline()
                words = line.decode().strip().split()
                action, raw_id = words[:2]
                index = int(raw_id)
                if not 0 <= index < args.tcp_count:
                    raise ValueError('invalid device')
                if action == 'silence':
                    args.silenced.add(index)
                elif action == 'resume':
                    args.silenced.discard(index)
                    args.gates[index].set()
                elif action == 'hold':
                    args.gates[index].clear()
                elif action in ('delay', 'exception') and len(words) == 3:
                    value = int(words[2])
                    if value < 0 or (action == 'exception' and not 1 <= value <= 255):
                        raise ValueError('invalid value')
                    args.device_faults[index][action] = value
                elif action in ('garbage', 'partial', 'wrong-tx'):
                    args.device_faults[index][action] = True
                elif action in ('drop', 'reset'):
                    for connection in tuple(args.connections[index]):
                        connection.close()
                    if action == 'reset':
                        args.gates[index].set()
                        args.device_faults[index].clear()
                        args.silenced.discard(index)
                        args.registers[index][:] = [0] * 1024
                else:
                    raise ValueError('invalid control command')
                writer.write(b'OK\n')
                await writer.drain()
            except (ValueError, UnicodeError):
                writer.write(b'ERROR\n')
            finally:
                writer.close()
                await writer.wait_closed()
        servers.append(await asyncio.start_server(control, '127.0.0.1', args.control_port))
    for index in range(args.tcp_count):
        registers = args.registers[index]
        async def client(r, w, registers=registers, index=index):
            await tcp_client(r, w, registers, args, index)
        servers.append(await asyncio.start_server(client, '127.0.0.1', args.base_port + index))
    print('READY', flush=True)
    try:
        await asyncio.Event().wait()
    finally:
        for server in servers:
            server.close()
        await asyncio.gather(*(server.wait_closed() for server in servers))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--tcp-count', type=int, default=1)
    parser.add_argument('--base-port', type=int, default=15020)
    parser.add_argument('--latency-ms', type=float, default=0)
    parser.add_argument('--jitter-ms', type=float, default=0)
    parser.add_argument('--serial', action='store_true')
    parser.add_argument('--control-port', type=int, default=0, help='optional loopback admin: silence ID / resume ID')
    parser.add_argument('--fault', choices=['drop-connection', 'delay', 'garbage-response'])
    parser.add_argument('--fault-device', type=int, default=0)
    parser.add_argument('--fault-delay-ms', type=float, default=5000)
    try:
        args = parser.parse_args()
        args.silenced = set()
        args.device_faults = [dict() for _ in range(args.tcp_count)]
        args.connections = [set() for _ in range(args.tcp_count)]
        args.registers = [[0] * 1024 for _ in range(args.tcp_count)]
        asyncio.run(main(args))
    except KeyboardInterrupt:
        pass
