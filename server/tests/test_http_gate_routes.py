# SPDX-FileCopyrightText: Copyright 2026 shadNet Project
# SPDX-License-Identifier: GPL-2.0-or-later
"""A hosted shadnet's HTTP ports (http_gate.h) answer only addresses with a signed-in link, as one
of the players linked there: a game linked from 127.0.0.1 over the real UDP link, its HTTP from
127.0.0.1 and a stranger's from 127.0.0.2.

Needs a built shadnet: SHADNET_EXE=<path to shadnet>. Skipped without it, or where 127.0.0.2
can't be bound (only Linux routes all of 127/8 to loopback)."""

import hashlib
import http.client
import json
import os
import socket
import sqlite3
import subprocess
import tempfile
import time
import unittest

SHADNET = os.environ.get("SHADNET_EXE", "")
HEADER = b"\xff\xff\xff\xffSHLK"
HELLO, WELCOME, COOKIE, WORK = 1, 2, 9, 10
BUILD = b"gatetest"


def free_port(kind=socket.SOCK_STREAM):
    with socket.socket(socket.AF_INET, kind) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def can_bind(address):
    try:
        with socket.socket() as s:
            s.bind((address, 0))
        return True
    except OSError:
        return False


def text(value):
    return bytes([len(value)]) + value


def hello(online_id, secret, cookie=b"", work=None, nonce=b"\x01" * 16):
    # Link version 3 (coop/link_auth.h): the cookie the server gave, a nonce, and the secret
    # once there is a cookie; hosted, a new account's work after it.
    return (HEADER + bytes([HELLO, 3]) + cookie.ljust(16, b"\0") + nonce +
            text(online_id) + text(BUILD) + text(secret if cookie else b"") +
            (b"" if work is None else work.to_bytes(8, "little")))


def solve_work(cookie, online_id, bits):
    nonce = 0
    while True:
        digest = int.from_bytes(hashlib.sha256(cookie + online_id + nonce.to_bytes(8, "little"))
                                .digest(), "big")
        if digest >> (256 - bits) == 0:
            return nonce
        nonce += 1


class Response:
    def __init__(self, status, body):
        self.status = status
        self.body = body

    def json(self):
        return json.loads(self.body)


def send(port, method, path, body=None, source="127.0.0.1", headers=None):
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=10,
                                      source_address=(source, 0))
    data = None
    headers = dict(headers or {})
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    conn.request(method, path, body=data, headers=headers)
    resp = conn.getresponse()
    result = Response(resp.status, resp.read())
    conn.close()
    return result


@unittest.skipUnless(SHADNET and os.path.exists(SHADNET), "SHADNET_EXE not set")
@unittest.skipUnless(can_bind("127.0.0.2"), "127.0.0.2 can't be bound here")
class HostedHttpGate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        work = cls.dir.name
        os.makedirs(os.path.join(work, "db"))
        cls.backend = free_port()
        cls.webapi = free_port()
        cls.link = free_port(socket.SOCK_DGRAM)
        config = {
            "Host": "127.0.0.1", "UnsecuredPort": free_port(), "MatchingUdpPort": cls.link,
            "WebApiPort": cls.webapi, "BackendPort": cls.backend,
            "BackendStore": os.path.join(work, "content.db").replace("\\", "/"),
            "BackendFolder": work.replace("\\", "/"), "BloodborneSeamlessCoop": "true",
            "Hosted": "true", "HostedBuild": BUILD.decode(), "StatsPort": free_port(),
            "HostedSignupWorkBits": 8,
        }
        with open(os.path.join(work, "shadnet.cfg"), "w") as f:
            f.write("[General]\n" + "".join(f"{k}={v}\n" for k, v in config.items()))
        cls.log = open(os.path.join(work, "shadnet.log"), "wb")
        cls.proc = subprocess.Popen([SHADNET], env=dict(os.environ, SHADNET_HOME=work),
                                    stdout=cls.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 20
        while True:
            try:
                send(cls.backend, "GET", "/status")
                break
            except OSError:
                if time.time() > deadline or cls.proc.poll() is not None:
                    cls.tearDownClass()
                    raise RuntimeError("shadnet did not start")
                time.sleep(0.1)
        # The game's link, from 127.0.0.1: hello until welcomed (the password is checked on
        # another thread). Kept open, so the link stays.
        cls.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        cls.udp.bind(("127.0.0.1", 0))
        cls.udp.settimeout(0.5)
        cookie = b""
        work = None
        nonce = b"\x01" * 16
        for _ in range(40):
            cls.udp.sendto(hello(b"pgate01", b"secret-of-pgate01", cookie, work, nonce),
                           ("127.0.0.1", cls.link))
            try:
                answer = cls.udp.recv(2048)
            except socket.timeout:
                continue
            if answer.startswith(HEADER) and answer[8] == COOKIE:
                cookie = answer[9:25]
            if answer.startswith(HEADER) and answer[8] == WORK:
                # A new account: the work, under a new nonce (as the game does).
                work = solve_work(cookie, b"pgate01", answer[9])
                nonce = b"\x02" * 16
            if answer.startswith(HEADER) and answer[8] == WELCOME and answer[9] == 1:
                break
        else:
            cls.tearDownClass()
            raise RuntimeError("the link was not welcomed")

    @classmethod
    def tearDownClass(cls):
        cls.proc.terminate()
        try:
            cls.proc.wait(10)
        except subprocess.TimeoutExpired:
            cls.proc.kill()
        cls.log.close()
        if getattr(cls, "udp", None):
            cls.udp.close()
        cls.dir.cleanup()

    def login(self, account, source="127.0.0.1"):
        return send(self.backend, "POST", "/login",
                    {"MessageId": "LoginRequest", "PlatformAccountId": account}, source)

    def test_backend_answers_the_linked_player_only(self):
        mine = self.login("pgate01")
        self.assertEqual(mine.status, 200)
        user_id = mine.json()["UserId"]
        self.assertEqual(self.login("pGATE01").status, 200)  # one account, whatever the case
        self.assertEqual(self.login("pnotlinked").status, 403)
        self.assertEqual(self.login("pgate01", source="127.0.0.2").status, 403)
        service = {"MessageId": "SyncCharaIdRequest", "CharaIdNum": 1}
        self.assertEqual(send(self.backend, "POST", "/basic_utils/sync_chara_id",
                              dict(service, UserId=user_id)).status, 200)
        self.assertEqual(send(self.backend, "POST", "/basic_utils/sync_chara_id",
                              dict(service, UserId=user_id + 1)).status, 403)
        self.assertEqual(send(self.backend, "POST", "/basic_utils/sync_chara_id",
                              dict(service, UserId=user_id), source="127.0.0.2").status, 403)
        # The online check names nobody: a launcher asks it before its game runs.
        self.assertEqual(send(self.backend, "GET", "/bb-eu/ss.info", source="127.0.0.2").status,
                         200)

    def test_summons_answer_the_linked_player_only(self):
        user_id = self.login("pgate01").json()["UserId"]
        sign = {"MessageId": "SummonDataCreateRequest", "SessionId": "gate-session",
                "UserId": user_id, "AreaId": 1, "AreaRegionId": 2, "MatchingLevel": 3,
                "SummonData": "d", "SummonDataVersion": 3, "SummonType": 0}
        self.assertEqual(send(self.webapi, "POST", "/summon_messenger/create", sign).status, 200)
        other = dict(sign, UserId=user_id + 1, SessionId="someone-else")
        self.assertEqual(send(self.webapi, "POST", "/summon_messenger/create", other).status, 403)
        self.assertEqual(send(self.webapi, "POST", "/summon_messenger/create", sign,
                              source="127.0.0.2").status, 403)
        removal = {"MessageId": "SummonDataRemoveRequest", "SessionId": "gate-session"}
        self.assertEqual(send(self.webapi, "POST", "/summon_messenger/delete", removal,
                              source="127.0.0.2").status, 403)
        # A request that changes a sign names its player: without a UserId it could act on
        # anyone's sign (the party header is the game's to say).
        self.assertEqual(send(self.webapi, "POST", "/summon_messenger/delete", removal).status,
                         403)
        claim = {"MessageId": "SummonDataSummonRequest", "SessionId": "gate-session",
                 "TargetUserId": user_id}
        self.assertEqual(send(self.webapi, "POST", "/summon_messenger/request", claim).status,
                         403)
        self.assertEqual(send(self.webapi, "POST", "/summon_messenger/request",
                              dict(claim, UserId=user_id)).status, 200)
        self.assertEqual(send(self.webapi, "POST", "/summon_messenger/delete",
                              dict(removal, UserId=user_id)).status, 200)
        # The launcher's probe for the summon service (anything but 404) still sees it.
        self.assertNotEqual(send(self.webapi, "POST", "/summon_messenger/create",
                                 source="127.0.0.2").status, 404)

    def test_token_routes_answer_the_linked_player_only(self):
        with sqlite3.connect(os.path.join(self.dir.name, "db", "shadnet.db")) as db:
            token = db.execute("SELECT token FROM account WHERE username='pgate01'").fetchone()[0]
        bearer = {"Authorization": "Bearer " + token}
        mine = send(self.webapi, "GET", "/v1/users/me/profile", headers=bearer)
        self.assertNotIn(mine.status, (401, 403))
        self.assertEqual(send(self.webapi, "GET", "/v1/users/me/profile", headers=bearer,
                              source="127.0.0.2").status, 403)
        self.assertEqual(send(self.webapi, "GET", "/v1/users/me/profile",
                              headers={"Authorization": "Bearer nope"}).status, 401)

    def test_reports_are_off(self):
        self.assertEqual(send(self.webapi, "POST", "/bb/report/crash.dmp", None,
                              headers={"X-Player": "pgate01", "X-Secret": "x"}).status, 404)


if __name__ == "__main__":
    unittest.main()
