"""Run the real Tcl CGI with a stubbed HTTP transport."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / 'addon/src/addon/update-check.cgi'
TCLSH = os.environ.get('TCLSH', 'tclsh')
BASE = 'https://github.com/sruetzler/HB-SR-Devices-AddOn/releases/download'


@unittest.skipUnless(shutil.which(TCLSH), 'tclsh required')
class UpdateCheckTest(unittest.TestCase):
    def run_cgi(self, release=None, query='', failure=False):
        # hex encoding keeps fixtures separate from executable Tcl syntax.
        payload = json.dumps(release).encode().hex()
        setup = '''
rename exec original_exec
proc exec {args} {
  if {[string compare [lindex $args 0] "/usr/bin/wget"] != 0 ||
      [string compare [lindex $args end-2] "https://api.github.com/repos/sruetzler/HB-SR-Devices-AddOn/releases/latest"] != 0} {
    puts stderr "Unexpected HTTP command"
    exit 1
  }
'''
        setup += 'error "network failure"\n' if failure else f'return [binary format H* {payload}]\n'
        setup += '}\n'
        setup += f'set env(QUERY_STRING) [binary format H* {{{query.encode().hex()}}}]\n'
        result = subprocess.run([TCLSH], input=setup + SCRIPT.read_text(), text=True, capture_output=True, check=True)
        self.assertEqual(result.stderr, '')
        return result.stdout

    def release(self, tag='v0.04'):
        return {'tag_name': tag, 'assets': [{'browser_download_url': f'{BASE}/{tag}/hb-sr-devices-addon.tgz'}]}

    def test_version_with_and_without_v(self):
        for tag in ('v0.04', '0.04', 'v1.2.3'):
            self.assertTrue(self.run_cgi(self.release(tag)).endswith('\n\n' + tag.lstrip('v') + '\n'))

    def test_download_redirects_to_exact_release_asset(self):
        output = self.run_cgi(self.release(), 'cmd=download')
        self.assertIn('Status: 302 Found', output)
        self.assertIn(f'Location: {BASE}/v0.04/hb-sr-devices-addon.tgz\n', output)

    def test_failures_do_not_advertise_or_download_update(self):
        for release in ({'message': 'Not Found'}, {'tag_name': 'v0.04', 'assets': []},
                        self.release('v0.04-rc1'), self.release('bad'), None):
            self.assertTrue(self.run_cgi(release).endswith('n/a\n'))
            self.assertIn('Status: 503', self.run_cgi(release, 'cmd=download'))
        self.assertTrue(self.run_cgi(failure=True).endswith('n/a\n'))
        self.assertIn('Status: 503', self.run_cgi(query='cmd=download', failure=True))

    def test_query_cannot_override_urls_or_version(self):
        query = 'cmd=download&checkURL=https://evil.invalid&downloadURL=https://evil.invalid&newversion=99'
        output = self.run_cgi(self.release(), query)
        self.assertIn(f'Location: {BASE}/v0.04/', output)
        self.assertNotIn('evil.invalid', output)

    def test_package_after_other_assets(self):
        release = self.release()
        release['assets'].insert(0, {'browser_download_url': f'{BASE}/v0.04/checksum.txt'})
        self.assertTrue(self.run_cgi(release).endswith('0.04\n'))
        self.assertIn('Status: 302', self.run_cgi(release, 'cmd=download'))

    def test_wrong_release_asset_and_body_text_are_ignored(self):
        release = self.release()
        release['assets'][0]['browser_download_url'] = f'{BASE}/v0.03/hb-sr-devices-addon.tgz'
        release['body'] = json.dumps(self.release())
        self.assertTrue(self.run_cgi(release).endswith('n/a\n'))


@unittest.skipUnless(shutil.which(TCLSH) and shutil.which('busybox'), 'Tcl and BusyBox required')
class BusyBoxTransportTest(unittest.TestCase):
    def test_real_busybox_transport(self):
        import http.server
        import threading

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                self.wfile.write(json.dumps({
                    'tag_name': '1.0',
                    'assets': [{'browser_download_url': f'{BASE}/1.0/hb-sr-devices-addon.tgz'}]
                }).encode())

            def log_message(self, *args):
                pass

        server = http.server.HTTPServer(('127.0.0.1', 0), Handler)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            # Use the actual CGI arguments with BusyBox, replacing only the host.
            setup = f'''rename exec original_exec
proc exec {{args}} {{
  set args [lreplace $args 0 0 {{{shutil.which('busybox')}}} wget]
  set args [lreplace $args end-2 end-2 http://127.0.0.1:{server.server_port}/]
  return [eval original_exec $args]
}}
'''
            result = subprocess.run([TCLSH], input=setup + SCRIPT.read_text(),
                                    text=True, capture_output=True, check=True, timeout=10)
            self.assertTrue(result.stdout.endswith('1.0\n'), result.stdout + result.stderr)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()
