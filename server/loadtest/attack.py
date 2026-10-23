#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Abuse against a hosted shadNet (docs/HOSTED_SERVER.md), to run beside a load test (bbload):

    attack.py signin-flood --rate 2000 --seconds 30 [--addresses 1]
        hellos with a new player id each, from one loopback address (or spread over many, as a
        botnet would): every new id is an account to make and a password to hash
    attack.py packet-flood --rate 20000 --seconds 30
        one signed-in game sending datagrams as fast as it can

Prints one JSON line: what it sent and what came back."""
import argparse
import json
import random
import socket
import struct
import time

HEADER = b"\xff\xff\xff\xffSHLK"
HELLO, WELCOME, DATA, PING, PONG = 1, 2, 3, 4, 5


def text(value: bytes) -> bytes:
    return bytes([len(value)]) + value


def hello(online_id: bytes, secret: bytes, build: bytes = b"bbload") -> bytes:
    return HEADER + bytes([HELLO, 2]) + text(online_id) + text(build) + text(secret)


def data(peer_ip: int, peer_port: int, frame: bytes) -> bytes:
    return HEADER + bytes([DATA]) + struct.pack(">IH", peer_ip, peer_port) + frame


def welcome_of(datagram: bytes):
    if len(datagram) >= 11 and datagram.startswith(HEADER) and datagram[8] == WELCOME:
        return datagram[9] != 0, datagram[11:11 + datagram[10]].decode(errors="replace")
    return None


def signin_flood(args):
    sockets = []
    for index in range(args.addresses):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((f"127.200.{index // 250}.{index % 250 + 2}", 0))
        s.setblocking(False)
        sockets.append(s)
    sent = welcomed = refused = 0
    reasons = {}
    start = time.time()
    interval = 1.0 / args.rate
    next_at = start
    tag = random.randrange(1 << 30)
    while time.time() - start < args.seconds:
        now = time.time()
        if now >= next_at:
            s = sockets[sent % len(sockets)]
            online_id = f"bot{tag % 1000}x{sent}".encode()[:16]
            s.sendto(hello(online_id, b"pw-" + online_id), (args.server, args.port))
            sent += 1
            next_at += interval
        for s in sockets:
            try:
                while True:
                    answer = welcome_of(s.recv(2048))
                    if answer:
                        if answer[0]:
                            welcomed += 1
                        else:
                            refused += 1
                            reasons[answer[1]] = reasons.get(answer[1], 0) + 1
            except BlockingIOError:
                pass
        wait = next_at - time.time()
        if wait > 0:
            time.sleep(min(0.001, wait))
    return {"attack": "signin-flood", "addresses": args.addresses, "hellos_sent": sent,
            "welcomed": welcomed, "refused": refused, "reasons": reasons,
            "seconds": round(time.time() - start, 1)}


def packet_flood(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.201.0.2", 0))
    s.settimeout(0.5)
    online_id = f"flood{random.randrange(10000)}".encode()
    welcomed = False
    for _ in range(20):  # the sign-in is checked on another thread: hello until welcomed
        s.sendto(hello(online_id, b"pw-" + online_id), (args.server, args.port))
        try:
            answer = welcome_of(s.recv(2048))
            if answer and answer[0]:
                welcomed = True
                break
        except socket.timeout:
            pass
    s.setblocking(False)
    frame = b"\x0e\x4a\x0e\x4a" + b"X" * 600
    victim = (10 << 24) | 0x123456
    sent = pongs = 0
    start = time.time()
    interval = 1.0 / args.rate
    next_at = start
    last_ping = 0.0
    pongs_after_ban_check = 0
    while time.time() - start < args.seconds:
        now = time.time()
        while now >= next_at and sent < args.rate * args.seconds:
            s.sendto(data(victim, 3658, frame), (args.server, args.port))
            sent += 1
            next_at += interval
        if now - last_ping >= 0.5:
            last_ping = now
            s.sendto(HEADER + bytes([PING]) + struct.pack("<IQ", 1, 0), (args.server, args.port))
        try:
            while True:
                datagram = s.recv(2048)
                if datagram.startswith(HEADER) and datagram[8] == PONG:
                    pongs += 1
                    if now - start > args.seconds / 2:
                        pongs_after_ban_check += 1
        except BlockingIOError:
            pass
        time.sleep(0.0005)
    return {"attack": "packet-flood", "welcomed": welcomed, "datagrams_sent": sent,
            "rate": round(sent / (time.time() - start)), "pongs": pongs,
            "pongs_in_second_half": pongs_after_ban_check}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("attack", choices=["signin-flood", "packet-flood"])
    parser.add_argument("--server", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=31314)
    parser.add_argument("--rate", type=float, default=2000)
    parser.add_argument("--seconds", type=float, default=30)
    parser.add_argument("--addresses", type=int, default=1)
    args = parser.parse_args()
    result = signin_flood(args) if args.attack == "signin-flood" else packet_flood(args)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
