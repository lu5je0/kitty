from base64 import standard_b64encode

from kitty_tests import BaseTest, parse_bytes


def tui_bridge(method):
    """The exact message ~/.dotfiles/zsh/vi-im-switch.zsh and the nvim osc
    backend emit."""
    payload = ('{"id":1,"module":"ime","method":"%s","params":{}}' % method).encode()
    return b'\x1b]1337;SetUserVar=tui-bridge=' + standard_b64encode(payload) + b'\x07'


class TestDisableIME(BaseTest):

    def test_tui_bridge_osc_toggles_ime(self):
        s = self.create_screen()
        self.assertFalse(s.ime_disabled)

        parse_bytes(s, tui_bridge('normal'))
        self.assertTrue(s.ime_disabled)

        parse_bytes(s, tui_bridge('insert'))
        self.assertFalse(s.ime_disabled)

    def test_ris_clears_ime_disabled(self):
        s = self.create_screen()
        parse_bytes(s, tui_bridge('normal'))
        self.assertTrue(s.ime_disabled)
        parse_bytes(s, b'\x1bc')
        self.assertFalse(s.ime_disabled)

    def test_unrelated_osc_1337_is_not_swallowed(self):
        s = self.create_screen()
        c = s.callbacks

        # our messages are consumed before the normal OSC 1337 handler
        c.clear()
        parse_bytes(s, tui_bridge('normal'))
        self.ae([], c.notifications)

        # everything else still reaches it
        c.clear()
        parse_bytes(s, b'\x1b]1337;SetUserVar=FOO=' + standard_b64encode(b'bar') + b'\x07')
        self.ae([(1337, 'SetUserVar=FOO=YmFy')], c.notifications)
        self.assertTrue(s.ime_disabled)  # and does not disturb our state

    def test_malformed_tui_bridge_payloads_are_ignored(self):
        s = self.create_screen()
        for payload in (
            b'\x1b]1337;SetUserVar=tui-bridge=not!base64\x07',
            b'\x1b]1337;SetUserVar=tui-bridge=\x07',
            # right envelope, wrong module
            b'\x1b]1337;SetUserVar=tui-bridge=' + standard_b64encode(
                b'{"id":1,"module":"clipboard","method":"normal","params":{}}') + b'\x07',
            # right module, unknown method
            b'\x1b]1337;SetUserVar=tui-bridge=' + standard_b64encode(
                b'{"id":1,"module":"ime","method":"bogus","params":{}}') + b'\x07',
        ):
            parse_bytes(s, payload)
            self.assertFalse(s.ime_disabled, f'unexpectedly disabled by {payload!r}')
