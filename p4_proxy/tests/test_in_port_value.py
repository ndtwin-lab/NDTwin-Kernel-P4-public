"""
An in_port the 5-tuple key cannot hold is the caller's error on every write endpoint.

[Co-developed with claude code -- Adam]

flow_5tuple's ingress-port key is bit<9>. The 5-tuple writer now refuses a value above 511 with
OverflowError while it builds the request, which is before any of the three verbs' try blocks
(they catch grpc.RpcError only), and api_routes catches only UnsupportedMatchError -- so,
unchecked, add, delete and modify all answered **500** for `{"in_port": 512}`. Before the mask
fix the same value reached the switch, which refused it: 200 {"status": "error"} on add and
delete, 400 on modify. `topology_manager.check_match_values` now runs the real encoder over
every in_port spelling first, so the answer is a 400 and the switch is not written.

The other numeric 5-tuple keys (ip_proto, tp_src, tp_dst) are deliberately NOT covered. The last
test only pins that check_match_values leaves them alone; it does not pin what they answer.

These tests drive the real handlers, the real TopologyManager and the real P4RuntimeClient over
a recording stub (the client of tests/test_p4_client_writes.py), so a refusal that is issued
after the write would show as a request on the stub.

unittest rather than pytest: tools/test_workflow/l1_unit_tests.sh executes each file directly.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

try:
    from fastapi import HTTPException

    from proxy_agent import api_routes
    from proxy_agent.topology_manager import TopologyManager
    from tests.test_p4_client_writes import RecordingStub, a_client

    HAVE_P4RUNTIME = True
except ImportError:  # pragma: no cover -- depends on the interpreter L1 picks
    HAVE_P4RUNTIME = False


class FakeRequest:
    def __init__(self, body):
        self._raw = json.dumps(body).encode()

    async def json(self):
        return json.loads(self._raw)


@unittest.skipUnless(HAVE_P4RUNTIME, "P4Runtime protobufs not available in this interpreter")
class AnInPortTheKeyCannotHoldTest(unittest.TestCase):
    HANDLERS = (("add", "add_flow_entry", True), ("delete", "delete_flow_entry", False),
                ("modify", "modify_flow_entry", True))
    #: 512..65535 fit the two bytes the key is sent in and used to reach the switch; the rest
    #: never fit those bytes. A string that is not a number, and the empty value, are 500s too.
    BAD = (512, 600, 65535, 65536, 70000, -1, "abc", "", None, 2.5e30)

    def setUp(self):
        self.stub = RecordingStub()
        self.topo = TopologyManager()
        self.topo.add_switch(1, a_client(self.stub))
        api_routes.topology = self.topo

    def tearDown(self):
        api_routes.topology = None

    def body(self, in_port):
        return {"dpid": 1, "match": {"in_port": in_port, "nw_dst": "10.0.0.4"},
                "actions": [{"type": "OUTPUT", "port": 3}], "priority": 100}

    def call(self, handler_name, in_port):
        return asyncio.run(getattr(api_routes, handler_name)(FakeRequest(self.body(in_port))))

    def test_every_write_verb_answers_400_for_an_in_port_that_does_not_fit_nine_bits(self):
        for verb, handler, _ in self.HANDLERS:
            for bad in self.BAD:
                with self.subTest(verb=verb, in_port=bad):
                    with self.assertRaises(HTTPException) as caught:
                        self.call(handler, bad)
                    self.assertEqual(caught.exception.status_code, 400)
                    detail = caught.exception.detail
                    self.assertEqual(detail["fields"], ["in_port"])
                    self.assertIn("0 to 511", detail["message"])
                    self.assertIn("No rule was", detail["message"])

    def test_the_switch_is_not_written_for_any_of_them(self):
        for verb, handler, _ in self.HANDLERS:
            for bad in self.BAD:
                with self.subTest(verb=verb, in_port=bad):
                    with self.assertRaises(HTTPException):
                        self.call(handler, bad)
        self.assertEqual(self.stub.requests, [], "a refused in_port reached the wire")

    def test_the_message_names_the_value_that_was_sent(self):
        with self.assertRaises(HTTPException) as caught:
            self.call("add_flow_entry", 512)
        self.assertIn("512", caught.exception.detail["message"])
        self.assertNotIn("IPv4", caught.exception.detail["message"])

    def test_a_valid_in_port_still_goes_through_on_every_verb(self):
        for verb, handler, answers_success in self.HANDLERS:
            for good in (0, 2, 511, "7"):
                with self.subTest(verb=verb, in_port=good):
                    before = len(self.stub.requests)
                    answer = self.call(handler, good)
                    if answers_success:
                        self.assertEqual(answer["status"], "success")
                    self.assertEqual(len(self.stub.requests), before + 1,
                                     "a valid in_port did not reach the switch")

    def test_the_widest_in_port_is_sent_with_a_nine_bit_mask(self):
        self.call("add_flow_entry", 511)
        entry = self.stub.requests[-1].updates[0].entity.table_entry
        sent = {m.field_id: (m.ternary.value, m.ternary.mask) for m in entry.match}
        self.assertEqual(sent[1], (b"\x01\xff", b"\x01\xff"))

    def test_the_other_numeric_keys_are_left_alone_by_this_check(self):
        # Not changed by the in_port check. Pinned so that widening it is a decision and not an
        # accident.
        from proxy_agent.topology_manager import check_match_values
        for match in ({"nw_proto": 300}, {"tp_dst": 70000}, {"tp_src": "abc"}):
            with self.subTest(match=match):
                self.assertIsNone(check_match_values(match, "installed"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
