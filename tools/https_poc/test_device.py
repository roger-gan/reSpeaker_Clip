"""Exercise the USB response parser without a physical serial device."""
import unittest
from unittest.mock import patch

from device import exchange


class ExchangeTest(unittest.TestCase):
    def exchange_bytes(self, chunks):
        with patch("device.os.write", side_effect=lambda fd, data: len(data)), \
                patch("device.select.select", return_value=([7], [], [])), \
                patch("device.os.read", side_effect=chunks):
            return exchange(7, "AT+UPLOAD?")

    def test_fragmented_reply(self):
        self.assertEqual(self.exchange_bytes([b'{"ok":tr', b'ue,"active":false}\n']),
                         {"ok": True, "active": False})

    def test_ignore_event_and_non_object_json(self):
        self.assertEqual(self.exchange_bytes([
            b'[]\nnull\n{"event":"state","state":"IDLE"}\n{"ok":true}\n'
        ]), {"ok": True})

    def test_non_boolean_ok_is_not_success(self):
        self.assertEqual(self.exchange_bytes([b'{"ok":"false"}\n{"ok":true}\n']),
                         {"ok": True})

    def test_rejection(self):
        with self.assertRaises(RuntimeError):
            self.exchange_bytes([b'{"ok":false,"error":-16}\n'])

    def test_disconnect(self):
        with self.assertRaisesRegex(RuntimeError, "disconnected"):
            self.exchange_bytes([b''])

    def test_oversized_response(self):
        with self.assertRaisesRegex(RuntimeError, "too large"):
            self.exchange_bytes([b'x' * 4097])

    def test_command_limit_before_write(self):
        with patch("device.os.write") as write:
            with self.assertRaises(ValueError):
                exchange(7, "X" * 254)
            write.assert_not_called()

    def test_timeout(self):
        with patch("device.os.write", return_value=12), \
                patch("device.time.monotonic", side_effect=[0, 11]):
            with self.assertRaises(TimeoutError):
                exchange(7, "AT+UPLOAD?")

    def test_partial_command_writes(self):
        with patch("device.os.write", side_effect=[3, 9]) as write, \
                patch("device.select.select", return_value=([7], [], [])), \
                patch("device.os.read", return_value=b'{"ok":true}\n'):
            self.assertEqual(exchange(7, "AT+UPLOAD?"), {"ok": True})
            self.assertEqual(write.call_args_list[0].args[1], b'AT+UPLOAD?\r\n')
            self.assertEqual(write.call_args_list[1].args[1], b'UPLOAD?\r\n')


if __name__ == "__main__":
    unittest.main()
