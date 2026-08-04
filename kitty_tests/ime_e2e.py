import os
import shutil
import time

from kitty_tests import BaseTest


class TestImeE2E(BaseTest):
    """Drives the real nvim config, so it is skipped where that is absent."""

    def _nvim(self):
        home = os.environ.get('KT_ORIGINAL_HOME') or os.path.expanduser('~')
        if not shutil.which('nvim'):
            self.skipTest('nvim not installed')
        if not os.path.isdir(os.path.join(home, '.dotfiles', 'vim', 'lua', 'lu5je0', 'misc', 'ime')):
            self.skipTest('author dotfiles not present')

        # test.py redirects HOME/XDG at a temp dir; point them back so nvim
        # actually loads the configuration under test
        env = os.environ.copy()
        env['TERM'] = 'xterm-kitty'
        env['HOME'] = home
        for k in ('XDG_CONFIG_HOME', 'XDG_CONFIG_DIRS', 'XDG_DATA_DIRS', 'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR'):
            env.pop(k, None)
        return self.create_pty(['nvim', '-n'], 30, 100, env=env)

    def _expect(self, pty, want, why, hold=0.75, timeout=20):
        def cur():
            return 'disabled' if pty.screen.ime_disabled else 'enabled'

        def pump(seconds):
            end = time.monotonic() + seconds
            while time.monotonic() < end:
                try:
                    pty.process_input_from_child(timeout=0.1)
                except Exception:
                    time.sleep(0.05)

        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if cur() == want:
                # a transient match is not good enough: require it to settle
                start, stable = time.monotonic(), True
                while time.monotonic() - start < hold:
                    pump(0.1)
                    if cur() != want:
                        stable = False
                        break
                if stable:
                    print(f'ok  {why}: {want}')
                    return
            pump(0.1)
        raise AssertionError(f'{why}: wanted {want}, settled on {cur()}')

    def test_nvim_drives_ime_mode(self):
        pty = self._nvim()
        self._expect(pty, 'disabled', 'startup (normal mode)')

        pty.write_to_child(b'i', flush=True)
        self._expect(pty, 'enabled', 'InsertEnter')

        pty.write_to_child(b'\x1b', flush=True)
        self._expect(pty, 'disabled', 'InsertLeave')

        pty.write_to_child(b':', flush=True)
        self._expect(pty, 'enabled', 'CmdlineEnter')

        pty.write_to_child(b'\x1b', flush=True)
        self._expect(pty, 'disabled', 'CmdlineLeave')

        # terminal mode goes through TermEnter/TermLeave, not InsertEnter
        pty.write_to_child(b':terminal\r', flush=True)
        self._expect(pty, 'disabled', 'TermOpen (still normal mode)')

        pty.write_to_child(b'i', flush=True)
        self._expect(pty, 'enabled', 'TermEnter')

        pty.write_to_child(b'\x1c\x0e', flush=True)  # ctrl-\ ctrl-n
        self._expect(pty, 'disabled', 'TermLeave')

        pty.write_to_child(b':qa!\r', flush=True)

    def test_quitting_restores_the_ime(self):
        # kitty bypasses the IME rather than switching the input source, so a
        # window left bypassed after nvim exits cannot be rescued by hand.
        pty = self._nvim()
        self._expect(pty, 'disabled', 'normal mode before quitting')
        pty.write_to_child(b':qa!\r', flush=True)
        self._expect(pty, 'enabled', 'after nvim exits')
