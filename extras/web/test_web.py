#!/usr/bin/env python3
"""Exercise the optional HTTP adapter against the real C++ engine.

Run from any directory with ``python3 extras/web/test_web.py``. The suite starts
an isolated server on an ephemeral loopback port and uses only the Python
standard library. No existing CLI session is contacted or modified.
"""

from collections import deque
from concurrent.futures import ThreadPoolExecutor
import importlib.util
import json
import os
from pathlib import Path
import queue
import re
import signal
import subprocess
import sys
import threading
import time
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen


class WebIntegrationTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server_output = deque(maxlen=60)
        lines = queue.Queue()
        server = Path(__file__).resolve().with_name("server.py")
        cls.process = subprocess.Popen(
            [sys.executable, "-u", str(server), "--port", "0"],
            cwd=server.parent,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            start_new_session=(os.name == "posix"),
        )
        cls.addClassCleanup(cls.stop_server)

        def collect_output():
            for line in cls.process.stdout:
                cls.server_output.append(line.rstrip())
                lines.put(line)

        cls.reader = threading.Thread(target=collect_output, daemon=True)
        cls.reader.start()
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            if cls.process.poll() is not None:
                raise RuntimeError(
                    "Server exited during startup:\n" + "\n".join(cls.server_output)
                )
            try:
                line = lines.get(timeout=0.2)
            except queue.Empty:
                continue
            match = re.search(r"http://127\.0\.0\.1:\d+", line)
            if match:
                cls.base_url = match.group(0)
                # Seeing the startup URL must mean the HTTP socket is ready.
                status, state = cls.http("GET", "/api/state")
                if status != 200 or not state.get("ok"):
                    raise RuntimeError(f"Unexpected initial state: {status} {state}")
                return
        raise RuntimeError(
            "Timed out waiting for server URL:\n" + "\n".join(cls.server_output)
        )

    @classmethod
    def stop_server(cls):
        if cls.process.poll() is None:
            if os.name == "posix":
                os.killpg(cls.process.pid, signal.SIGTERM)
            else:
                cls.process.terminate()
            try:
                cls.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                if os.name == "posix":
                    os.killpg(cls.process.pid, signal.SIGKILL)
                else:
                    cls.process.kill()
                cls.process.wait(timeout=5)
        cls.reader.join(timeout=2)
        cls.process.stdout.close()

    @classmethod
    def http(cls, method, path, payload=None, headers=None, raw=None):
        body = raw
        if payload is not None:
            body = json.dumps(payload).encode("utf-8")
        request_headers = {}
        if body is not None:
            request_headers["Content-Type"] = "application/json"
        request_headers.update(headers or {})
        request = Request(
            cls.base_url + path, data=body, headers=request_headers, method=method
        )
        try:
            response = urlopen(request, timeout=10)
        except HTTPError as error:
            response = error
        with response:
            return response.status, json.loads(response.read())

    def post(self, path, payload, expected_status=200):
        status, state = self.http("POST", path, payload)
        self.assertEqual(status, expected_status, state)
        self.assertEqual(state["ok"], expected_status == 200, state)
        if expected_status != 200:
            self.assertTrue(state["error"], state)
        return state

    def command(self, action, expected_status=200, **fields):
        return self.post(
            "/api/command", {"action": action, **fields}, expected_status
        )

    def state(self):
        status, state = self.http("GET", "/api/state")
        self.assertEqual(status, 200, state)
        self.assertTrue(state["ok"], state)
        return state

    def setUp(self):
        self.post("/api/reset", {})

    @staticmethod
    def order(state, order_id):
        return next(order for order in state["orders"] if order["id"] == str(order_id))

    @staticmethod
    def level_ids(state, side, level=0):
        return [order["id"] for order in state["book"][side][level]["orders"]]

    def assert_matching_unchanged(self, before, after):
        # Rejected requests may create an error event, but cannot alter matching.
        for field in ("session_id", "book", "orders", "trades", "metrics"):
            self.assertEqual(before[field], after[field], field)

    def test_assignment_example_fifo_partial_fills_and_market_remainders(self):
        self.command("limit", side="buy", price="10", quantity="100")
        self.command("limit", side="sell", price="20", quantity="100")
        self.command("limit", side="sell", price="20", quantity="200")
        state = self.command("market", side="buy", quantity="150")

        self.assertEqual(self.order(state, 2)["status"], "filled")
        self.assertEqual(self.order(state, 2)["remaining_quantity"], "0")
        self.assertEqual(self.order(state, 3)["remaining_quantity"], "150")
        self.assertEqual(self.level_ids(state, "sells"), ["3"])
        self.assertEqual(
            [(trade["resting_order_id"], trade["quantity"]) for trade in reversed(state["trades"])],
            [("2", "100"), ("3", "50")],
        )
        self.assertTrue(all(trade["price"] == "2000" for trade in state["trades"]))

        state = self.command("market", side="buy", quantity="200")
        self.assertEqual(self.order(state, 5)["status"], "cancelled")
        self.assertEqual(state["trades"][0]["quantity"], "150")
        state = self.command("market", side="sell", quantity="200")
        self.assertEqual(state["book"], {"buys": [], "sells": []})
        self.assertEqual(self.order(state, 6)["status"], "cancelled")
        self.assertEqual(state["trades"][0]["price"], "1000")
        self.assertEqual(state["trades"][0]["quantity"], "100")
        self.assertEqual(state["metrics"]["trade_count"], 4)
        self.assertEqual(state["metrics"]["executed_quantity"], "400")
        self.assertEqual(state["metrics"]["cancelled_orders"], 2)
        self.assertEqual(state["metrics"]["open_orders"], 0)

    def test_crossing_limit_uses_best_resting_prices_and_rests_remainder(self):
        self.command("limit", side="sell", price="10.50", quantity="40")
        self.command("limit", side="sell", price="10", quantity="25")
        before = self.state()
        preview = self.command("limit", expected_status=409, side="buy", price="11", quantity="100")
        self.assertEqual(self.state(), before)
        self.assertEqual(preview["confirmation_required"]["best_price"], "1000")
        state = self.command("limit", side="buy", price="11", quantity="100",
                             confirmation=preview["confirmation_required"]["token"])
        self.assertEqual(
            [(trade["price"], trade["quantity"], trade["resting_order_id"]) for trade in reversed(state["trades"])],
            [("1000", "25", "2"), ("1050", "40", "1")],
        )
        self.assertEqual(state["book"]["sells"], [])
        self.assertEqual(state["book"]["buys"][0]["price"], "1100")
        self.assertEqual(state["book"]["buys"][0]["total_quantity"], "35")
        self.assertEqual(self.order(state, 3)["status"], "active")

    def test_crossing_limit_equal_price_requires_consent_for_both_sides(self):
        for side, opposite in (("buy", "sell"), ("sell", "buy")):
            with self.subTest(side=side):
                self.post("/api/reset", {})
                self.command("limit", side=opposite, price="10", quantity="50")
                before = self.state()
                preview = self.command("limit", expected_status=409, side=side,
                                       price="10", quantity="20")
                self.assertEqual(self.state(), before)
                # Declining is simply not submitting a confirmation.
                preview = self.command("limit", expected_status=409, side=side,
                                       price="10", quantity="20")
                self.assertEqual(self.state(), before)
                after = self.command("limit", side=side, price="10", quantity="20",
                                     confirmation=preview["confirmation_required"]["token"])
                self.assertEqual(self.order(after, 2)["status"], "filled")
                self.assertEqual(self.order(after, 1)["remaining_quantity"], "30")

    def test_stale_or_changed_command_confirmation_requires_new_consent(self):
        self.command("limit", side="sell", price="10", quantity="50")
        preview = self.command("limit", expected_status=409, side="buy", price="11", quantity="20")
        token = preview["confirmation_required"]["token"]
        self.command("limit", side="sell", price="9", quantity="50")
        before = self.state()
        refreshed = self.command("limit", expected_status=409, side="buy", price="11",
                                 quantity="20", confirmation=token)
        self.assertEqual(self.state(), before)
        self.assertTrue(refreshed["confirmation_required"]["changed"])
        self.assertEqual(refreshed["confirmation_required"]["best_price"], "900")
        # Consent also binds quantity, side and price, not just the revision.
        changed = self.command("limit", expected_status=409, side="buy", price="11",
                               quantity="30", confirmation=refreshed["confirmation_required"]["token"])
        self.assertEqual(self.state(), before)
        after = self.command("limit", side="buy", price="11", quantity="30",
                             confirmation=changed["confirmation_required"]["token"])
        self.assertEqual(after["trades"][0]["price"], "900")
        self.assertEqual(after["trades"][0]["quantity"], "30")

    def test_confirmation_after_liquidity_removed_does_not_create_order(self):
        self.command("limit", side="sell", price="10", quantity="50")
        preview = self.command("limit", expected_status=409, side="buy", price="10", quantity="20")
        self.command("cancel", id="1")
        before = self.state()
        expired = self.command("limit", expected_status=409, side="buy", price="10",
                               quantity="20", confirmation=preview["confirmation_required"]["token"])
        self.assertNotIn("confirmation_required", expired)
        self.assertEqual(self.state(), before)

    def test_crossing_limit_validates_input_before_confirmation(self):
        self.command("limit", side="sell", price="10", quantity="50")
        before = self.state()
        for invalid in ({"quantity": "0"}, {"price": "-1"}, {"confirmation": True},
                        {"confirmation": None}, {"confirmation": {"revision": True}}):
            fields = {"side": "buy", "price": "10", "quantity": "20", **invalid}
            after = self.command("limit", expected_status=400, **fields)
            self.assertNotIn("confirmation_required", after)
            self.assert_matching_unchanged(before, after)

    def test_confirmation_from_previous_session_cannot_be_reused(self):
        self.command("limit", side="sell", price="10", quantity="50")
        preview = self.command("limit", expected_status=409, side="buy", price="10", quantity="20")
        self.post("/api/reset", {})
        self.command("limit", side="sell", price="10", quantity="50")
        before = self.state()
        self.command("limit", expected_status=409, side="buy", price="10", quantity="20",
                     confirmation=preview["confirmation_required"]["token"])
        self.assertEqual(self.state(), before)

    def test_confirmation_is_consumed_once_even_with_concurrent_requests(self):
        self.command("limit", side="sell", price="10", quantity="50")
        preview = self.command("limit", expected_status=409, side="buy", price="10", quantity="20")
        command = {"action": "limit", "side": "buy", "price": "10", "quantity": "20",
                   "confirmation": preview["confirmation_required"]["token"]}
        with ThreadPoolExecutor(max_workers=2) as pool:
            results = list(pool.map(lambda _: self.http("POST", "/api/command", command), range(2)))
        self.assertEqual(sorted(status for status, _ in results), [200, 409])
        self.assertEqual(self.state()["metrics"]["submitted"], 2)
        self.assertEqual(self.state()["metrics"]["executed_quantity"], "20")

    def test_cancellation_removes_liquidity_and_repeated_errors_do_not_mutate(self):
        self.command("limit", side="buy", price="10", quantity="100")
        state = self.command("cancel", id="1")
        self.assertEqual(state["book"], {"buys": [], "sells": []})
        self.assertEqual(self.order(state, 1)["status"], "cancelled")
        self.assertEqual(self.order(state, 1)["remaining_quantity"], "0")
        for order_id in ("1", "999"):
            with self.subTest(order_id=order_id):
                error_state = self.command("cancel", id=order_id, expected_status=400)
                self.assert_matching_unchanged(state, error_state)
        state = self.command("market", side="sell", quantity="50")
        self.assertEqual(state["trades"], [])
        self.assertEqual(self.order(state, 2)["status"], "cancelled")
        self.assertEqual(state["metrics"]["submitted"], 2)

    def test_atomic_amendment_changes_quantity_before_matching(self):
        self.command("limit", side="sell", price="10", quantity="150")
        self.command("limit", side="buy", price="9", quantity="100")
        state = self.command("amend", id="2", price="10", quantity="200")
        self.assertEqual(state["trades"][0]["quantity"], "150")
        self.assertEqual(self.order(state, 2)["original_quantity"], "100")
        self.assertEqual(self.order(state, 2)["remaining_quantity"], "50")
        self.assertEqual(state["book"]["buys"][0]["total_quantity"], "50")
        for change in ({"price": "10.25", "quantity": "0"}, {}):
            with self.subTest(change=change):
                rejected = self.command("amend", id="2", expected_status=400, **change)
                self.assert_matching_unchanged(state, rejected)

    def test_quantity_reduction_keeps_priority_increase_and_price_change_lose_it(self):
        self.command("limit", side="buy", price="10", quantity="100")
        self.command("limit", side="buy", price="10", quantity="100")
        self.command("limit", side="buy", price="9.99", quantity="50")
        state = self.command("amend", id="1", quantity="80")
        self.assertEqual(self.level_ids(state, "buys"), ["1", "2"])
        state = self.command("amend", id="1", quantity="200")
        self.assertEqual(self.level_ids(state, "buys"), ["2", "1"])
        state = self.command("market", side="sell", quantity="100")
        self.assertEqual(self.order(state, 2)["status"], "filled")
        self.assertEqual(self.order(state, 1)["remaining_quantity"], "200")
        state = self.command("amend", id="1", price="9.98")
        self.assertEqual([level["price"] for level in state["book"]["buys"]], ["999", "998"])
        self.assertEqual(self.level_ids(state, "buys", 0), ["3"])
        self.assertEqual(self.level_ids(state, "buys", 1), ["1"])

    def test_bid_and_offer_pegs_reprice_preserve_priority_and_cancel_without_reference(self):
        scenarios = (
            ("buy", "bid", "buys", "10", "10.10", "1010"),
            ("sell", "offer", "sells", "11", "10.50", "1050"),
        )
        for side, reference, book_side, first_price, better_price, cents in scenarios:
            with self.subTest(reference=reference):
                self.post("/api/reset", {})
                self.command("peg", side=side, reference=reference, quantity="50", expected_status=400)
                self.command("limit", side=side, price=first_price, quantity="100")
                initial = self.command("peg", side=side, reference=reference, quantity="50")
                sequence = self.order(initial, 2)["sequence"]
                state = self.command("limit", side=side, price=better_price, quantity="100")
                self.assertEqual(self.order(state, 2)["type"], "peg")
                self.assertEqual(self.order(state, 2)["peg_reference"], reference)
                self.assertEqual(self.order(state, 2)["price"], cents)
                self.assertEqual(self.order(state, 2)["sequence"], sequence)
                self.assertEqual(self.level_ids(state, book_side), ["2", "3"])
                self.assertTrue(any(event["kind"] == "repriced" for event in state["events"]))
                rejected = self.command("amend", id="2", price=first_price, expected_status=400)
                self.assert_matching_unchanged(state, rejected)
                state = self.command("cancel", id="3")
                self.assertEqual(self.level_ids(state, book_side), ["1", "2"])
                self.assertEqual(self.order(state, 2)["price"], self.order(state, 1)["price"])
                state = self.command("cancel", id="1")
                self.assertEqual(state["book"][book_side], [])
                self.assertEqual(self.order(state, 2)["status"], "cancelled")
                self.assertEqual(state["metrics"]["cancelled_orders"], 3)

    def test_pegs_refresh_after_the_entire_aggressive_order(self):
        self.command("limit", side="buy", price="10", quantity="10")
        self.command("peg", side="buy", reference="bid", quantity="20")
        self.command("limit", side="buy", price="9", quantity="20")
        state = self.command("market", side="sell", quantity="15")
        self.assertEqual(
            [(trade["resting_order_id"], trade["price"], trade["quantity"]) for trade in reversed(state["trades"])],
            [("1", "1000", "10"), ("2", "1000", "5")],
        )
        self.assertEqual(self.order(state, 2)["price"], "900")
        self.assertEqual(self.order(state, 2)["remaining_quantity"], "15")
        self.assertEqual(self.level_ids(state, "buys"), ["2", "3"])

    def test_exact_decimal_strings_and_invalid_inputs(self):
        state = self.command("limit", side="buy", price="9999999999.99", quantity="1000000000000")
        self.assertEqual(self.order(state, 1)["price"], "999999999999")
        self.assertEqual(self.order(state, 1)["remaining_quantity"], "1000000000000")
        invalid = [
            {"action": "limit", "side": "buy", "price": price, "quantity": "1"}
            for price in ("1e2", "1.001", "1,00", "NaN", "0", "-1", "10000000000.01", 10, True)
        ] + [
            {"action": "market", "side": "buy", "quantity": quantity}
            for quantity in ("0", "-1", "1.5", "1e2", "1000000000001", 1, True)
        ] + [
            {"action": "limit", "side": "unknown", "price": "10", "quantity": "1"},
            {"action": "peg", "side": "sell", "reference": "bid", "quantity": "1"},
            {"action": "cancel", "id": "1\nstate"},
            {"action": "unknown"},
            {},
            [],
        ]
        for payload in invalid:
            with self.subTest(payload=payload):
                rejected = self.post("/api/command", payload, expected_status=400)
                self.assert_matching_unchanged(state, rejected)
        status, rejected = self.http("POST", "/api/command", raw=b"{broken")
        self.assertEqual(status, 400)
        self.assert_matching_unchanged(state, rejected)
        state = self.command("limit", side="buy", price="0.01", quantity="1")
        self.assertEqual(self.order(state, 2)["price"], "1")

    def test_demo_requires_a_new_session_and_reset_discards_its_history(self):
        empty = self.state()
        state = self.post("/api/demo", {})
        self.assertTrue(state["book"]["buys"])
        self.assertTrue(state["book"]["sells"])
        self.assertGreater(state["metrics"]["submitted"], 0)
        rejected = self.post("/api/demo", {}, expected_status=409)
        self.assert_matching_unchanged(state, rejected)

        reset = self.post("/api/reset", {})
        self.assertNotEqual(reset["session_id"], empty["session_id"])
        self.assertEqual(reset["book"], {"buys": [], "sells": []})
        self.assertEqual(reset["orders"], [])
        self.assertEqual(reset["trades"], [])
        self.assertEqual(reset["metrics"]["submitted"], 0)
        self.assertEqual(reset["metrics"]["trade_count"], 0)
        self.command("limit", side="buy", price="10", quantity="1")
        state = self.command("cancel", id="1")
        self.assertEqual(state["book"]["buys"], [])
        rejected = self.post("/api/demo", {}, expected_status=409)
        self.assert_matching_unchanged(state, rejected)

    def test_http_boundary_rejects_foreign_origins_wrong_types_and_large_bodies(self):
        before = self.state()
        command = {"action": "limit", "side": "buy", "price": "10", "quantity": "1"}
        cases = (
            ({"Host": "example.invalid"}, 403),
            ({"Origin": "https://example.invalid"}, 403),
            ({"Origin": "http://127.0.0.1:1"}, 403),
            ({"Content-Type": "text/plain"}, 415),
            ({"Content-Type": ""}, 415),
        )
        for headers, expected_status in cases:
            with self.subTest(headers=headers):
                status, rejected = self.http("POST", "/api/command", command, headers)
                self.assertEqual(status, expected_status, rejected)
                self.assertFalse(rejected["ok"])
                self.assert_matching_unchanged(before, self.state())
        status, rejected = self.http(
            "POST", "/api/command", raw=json.dumps("x" * (17 * 1024)).encode("utf-8")
        )
        self.assertEqual(status, 413, rejected)
        self.assertFalse(rejected["ok"])
        self.assert_matching_unchanged(before, self.state())
        # The browser's ordinary same-origin request still works.
        status, state = self.http(
            "POST", "/api/command", command, {"Origin": self.base_url}
        )
        self.assertEqual(status, 200, state)
        self.assertEqual(self.level_ids(state, "buys"), ["1"])

    def test_old_tab_cannot_cancel_a_reused_id_after_another_tab_resets(self):
        previous = self.command("limit", side="buy", price="10", quantity="100")
        self.post("/api/reset", {})
        current = self.command("limit", side="buy", price="11", quantity="200")
        self.assertNotEqual(previous["session_id"], current["session_id"])
        self.assertEqual(current["orders"][0]["id"], "1")
        status, rejected = self.http(
            "POST", "/api/command", {"action": "cancel", "id": "1"},
            {"X-Session-ID": previous["session_id"]},
        )
        self.assertEqual(status, 409, rejected)
        self.assertFalse(rejected["ok"])
        self.assert_matching_unchanged(current, self.state())
        status, state = self.http(
            "POST", "/api/command", {"action": "cancel", "id": "1"},
            {"X-Session-ID": current["session_id"]},
        )
        self.assertEqual(status, 200, state)
        self.assertEqual(self.order(state, 1)["status"], "cancelled")

    def test_concurrent_clients_share_one_serialized_engine(self):
        count = 24

        def submit(_):
            return self.http(
                "POST", "/api/command",
                {"action": "limit", "side": "buy", "price": "10", "quantity": "1"},
            )

        with ThreadPoolExecutor(max_workers=8) as pool:
            responses = list(pool.map(submit, range(count)))
        for status, state in responses:
            self.assertEqual(status, 200, state)
        self.assertEqual(
            sorted(state["metrics"]["submitted"] for _, state in responses),
            list(range(1, count + 1)),
        )
        state = self.state()
        ids = [str(index) for index in range(1, count + 1)]
        self.assertEqual(self.level_ids(state, "buys"), ids)
        self.assertEqual(state["book"]["buys"][0]["total_quantity"], str(count))
        self.assertEqual(state["metrics"]["open_orders"], count)
        state = self.command("market", side="sell", quantity=str(count))
        self.assertEqual(
            [trade["resting_order_id"] for trade in reversed(state["trades"])], ids
        )
        self.assertEqual(state["metrics"]["trade_count"], count)
        self.assertEqual(state["metrics"]["executed_quantity"], str(count))
        self.assertEqual(state["metrics"]["open_orders"], 0)

    def test_history_is_bounded_while_session_metrics_remain_cumulative(self):
        count = 301
        self.command("limit", side="sell", price="10", quantity=str(count))
        for _ in range(count):
            state = self.command("market", side="buy", quantity="1")
        self.assertEqual(len(state["trades"]), 300)
        self.assertEqual(len(state["events"]), 300)
        self.assertEqual(state["metrics"]["trade_count"], count)
        self.assertEqual(state["metrics"]["executed_quantity"], str(count))
        self.assertEqual(state["metrics"]["submitted"], count + 1)
        self.assertEqual(state["metrics"]["open_orders"], 0)
        self.assertEqual(state["book"]["sells"], [])
        self.assertEqual(state["trades"][0]["aggressive_order_id"], str(count + 1))
        self.assertEqual(state["trades"][-1]["aggressive_order_id"], "3")


class BridgeRecoveryTest(unittest.TestCase):
    def test_process_failure_retains_last_snapshot_until_explicit_reset(self):
        # Own a separate C++ child so failure injection cannot touch a user's book.
        source = Path(__file__).resolve().with_name("server.py")
        spec = importlib.util.spec_from_file_location("web_server_under_test", source)
        server = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(server)
        session = server.Session(server.build_bridge())
        self.addCleanup(session.close)
        status, before = session.post(
            "/api/command",
            {"action": "limit", "side": "buy", "price": "10", "quantity": "100"},
        )
        self.assertEqual(status, 200)
        session.bridge.process.terminate()
        session.bridge.process.wait(timeout=5)
        status, failed = session.read_state()
        self.assertEqual(status, 503)
        self.assertFalse(failed["ok"])
        self.assertEqual(failed["book"], before["book"])
        self.assertEqual(failed["session_id"], before["session_id"])
        status, reset = session.post("/api/reset", {})
        self.assertEqual(status, 200)
        self.assertTrue(reset["ok"])
        self.assertEqual(reset["book"], {"buys": [], "sells": []})
        self.assertEqual(reset["orders"], [])
        self.assertNotEqual(reset["session_id"], before["session_id"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
