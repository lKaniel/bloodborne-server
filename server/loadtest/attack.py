#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""Abuse against a hosted shadNet (docs/HOSTED_SERVER.md), to run beside a load test (bbload):

    attack.py signin-flood --rate 2000 --seconds 30 [--addresses 1]
        hellos with a new player id each, from one loopback address (or spread over many, as a
        botnet would): every new id is an account to make and a password to hash
        (link version 3: each address does the cookie round trip first, as a real one can;
        --no-cookie sends only first hellos, all a spoofed source can do; hosted, a new id's
        hello must carry work (coop/link_auth.h): without --work the bots skip it, with it
        each does it, as a game does, on this one thread)
    attack.py packet-flood --rate 20000 --seconds 30 [--bad-tags]
        one signed-in game sending datagrams as fast as it can (--bad-tags: from its address with
        tags that do not open, as someone injecting into its link would)

Prints one JSON line: what it sent and what came back."""
import argparse
import hashlib
import hmac
import json
import os
import random
import socket
import struct
import time

HEADER = b"\xff\xff\xff\xffSHLK"
HELLO, WELCOME, DATA, PING, PONG, COOKIE, WORK = 1, 2, 3, 4, 5, 9, 10
LINK_VERSION = 3


def text(value: bytes) -> bytes:
    return bytes([len(value)]) + value


def hello(online_id: bytes, secret: bytes, build: bytes = b"bbload", cookie: bytes = b"",
          nonce: bytes = b"\0" * 16, work=None) -> bytes:
    cookie = cookie.ljust(16, b"\0")
    return (HEADER + bytes([HELLO, LINK_VERSION]) + cookie + nonce + text(online_id) +
            text(build) + text(secret if cookie.strip(b"\0") else b"") +
            (b"" if work is None else struct.pack("<Q", work)))


def solve_work(cookie: bytes, online_id: bytes, bits: int) -> int:
    """A nonce whose SHA-256(cookie, id, nonce) starts with `bits` zero bits."""
    prefix = hashlib.sha256(cookie + online_id)
    limit = 1 << (256 - bits)
    nonce = 0
    while True:
        h = prefix.copy()
        h.update(struct.pack("<Q", nonce))
        if int.from_bytes(h.digest(), "big") < limit:
            return nonce
        nonce += 1


def cookie_of(datagram: bytes):
    if len(datagram) >= 25 and datagram.startswith(HEADER) and datagram[8] == COOKIE:
        return datagram[9:25]
    return None


class Session:
    """The game's side of a link (server/coop/link_auth.h): its keys and counters."""

    def __init__(self, secret: bytes, nonce: bytes, cookie: bytes, online_id: bytes):
        context = hashlib.sha256(b"shadNet link 3" + nonce + cookie + online_id).digest()
        base = hmac.new(secret, context, hashlib.sha256).digest()
        self.up = hmac.new(base, b"up", hashlib.sha256).digest()
        self.down = hmac.new(base, b"down", hashlib.sha256).digest()
        self.counter = 0

    def seal(self, datagram: bytes) -> bytes:
        self.counter += 1
        counter = struct.pack("<Q", self.counter)
        return datagram + counter + hmac.new(self.up, datagram + counter,
                                             hashlib.sha256).digest()[:16]

    def open(self, datagram: bytes):
        if len(datagram) < 24:
            return None
        body, counter, tag = datagram[:-24], datagram[-24:-16], datagram[-16:]
        good = hmac.new(self.down, body + counter, hashlib.sha256).digest()[:16]
        return body if hmac.compare_digest(tag, good) else None


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
    sent = welcomed = refused = cookies = work_asked = worked = 0
    work_seconds = 0.0
    reasons = {}
    last_ids = {}  # socket index -> the id of its last first hello (its cookie answers that one)
    sessions = {}  # socket index -> the session of its last hello with a cookie
    cookies_of = {}  # socket index -> its cookie
    start = time.time()
    interval = 1.0 / args.rate
    next_at = start
    tag = random.randrange(1 << 30)
    while time.time() - start < args.seconds:
        now = time.time()
        if now >= next_at:
            s = sockets[sent % len(sockets)]
            online_id = f"bot{tag % 1000}x{sent}".encode()[:16]
            last_ids[sent % len(sockets)] = online_id
            s.sendto(hello(online_id, b"pw-" + online_id), (args.server, args.port))
            sent += 1
            next_at += interval
        for index, s in enumerate(sockets):
            try:
                while True:
                    datagram = s.recv(2048)
                    cookie = cookie_of(datagram)
                    if cookie:
                        cookies += 1
                        if not args.no_cookie:
                            online_id = last_ids[index]
                            nonce = os.urandom(16)
                            secret = b"pw-" + online_id
                            sessions[index] = Session(secret, nonce, cookie, online_id)
                            cookies_of[index] = cookie
                            s.sendto(hello(online_id, secret, cookie=cookie, nonce=nonce),
                                     (args.server, args.port))
                        continue
                    session = sessions.get(index)
                    body = session.open(datagram) if session else None
                    if body and body[8] == WORK:
                        work_asked += 1
                        if args.work:
                            # What a game does: the work, then the hello again with it, under
                            # a new nonce.
                            online_id = last_ids[index]
                            secret = b"pw-" + online_id
                            began = time.time()
                            nonce_work = solve_work(cookies_of[index], online_id, body[9])
                            work_seconds += time.time() - began
                            worked += 1
                            nonce = os.urandom(16)
                            sessions[index] = Session(secret, nonce, cookies_of[index], online_id)
                            s.sendto(hello(online_id, secret, cookie=cookies_of[index], nonce=nonce,
                                           work=nonce_work), (args.server, args.port))
                        continue
                    answer = welcome_of(body) if body else None
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
            "cookies": cookies, "work_asked": work_asked, "worked": worked,
            "work_seconds": round(work_seconds, 1), "welcomed": welcomed, "refused": refused,
            "reasons": reasons,
            "seconds": round(time.time() - start, 1)}


def packet_flood(args):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.201.0.2", 0))
    s.settimeout(0.5)
    online_id = f"flood{random.randrange(10000)}".encode()
    secret = b"pw-" + online_id
    welcomed = False
    session = None
    cookie = b""
    nonce = os.urandom(16)
    for _ in range(20):  # the sign-in is checked on another thread: hello until welcomed
        if cookie:
            session = Session(secret, nonce, cookie, online_id)
        s.sendto(hello(online_id, secret, cookie=cookie, nonce=nonce), (args.server, args.port))
        try:
            datagram = s.recv(2048)
            if cookie_of(datagram):
                cookie = cookie_of(datagram)
                continue
            body = session.open(datagram) if session else None
            answer = welcome_of(body) if body else None
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
            datagram = session.seal(data(victim, 3658, frame)) if session else b""
            if args.bad_tags:
                datagram = datagram[:-1] + bytes([datagram[-1] ^ 1])
            s.sendto(datagram, (args.server, args.port))
            sent += 1
            next_at += interval
        if now - last_ping >= 0.5 and session:
            last_ping = now
            s.sendto(session.seal(HEADER + bytes([PING]) + struct.pack("<IQ", 1, 0)),
                     (args.server, args.port))
        try:
            while True:
                datagram = s.recv(2048)
                if datagram.startswith(HEADER) and datagram[8] == PONG and session and session.open(datagram):
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
    parser.add_argument("--no-cookie", action="store_true", help="signin-flood: first hellos only")
    parser.add_argument("--bad-tags", action="store_true", help="packet-flood: tags that fail")
    parser.add_argument("--work", action="store_true",
                        help="signin-flood: do the work a new account needs (hosted)")
    args = parser.parse_args()
    result = signin_flood(args) if args.attack == "signin-flood" else packet_flood(args)
    print(json.dumps(result))


if __name__ == "__main__":
    main()
