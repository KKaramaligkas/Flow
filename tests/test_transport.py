import http.server
import pathlib
import ssl
import subprocess
import tempfile
import threading
import unittest


class Transport(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = tempfile.TemporaryDirectory()
        cls.cert = str(pathlib.Path(cls.folder.name, 'cert.pem'))
        key = str(pathlib.Path(cls.folder.name, 'key.pem'))
        subprocess.run(['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
                        '-keyout', key, '-out', cls.cert, '-days', '2', '-subj', '/CN=localhost',
                        '-addext', 'subjectAltName=DNS:localhost'], check=True, capture_output=True)
        cls.requests = []
        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass
            def do_GET(self):
                cls.requests.append((self.path, self.connection.version() if isinstance(self.connection, ssl.SSLSocket) else None))
                targets = {'/downgrade': f'http://localhost:{cls.http.server_port}/target',
                           '/upgrade': f'https://localhost:{cls.https.server_port}/target',
                           '/file': 'file:///etc/passwd', '/loop': '/loop'}
                if self.path in targets:
                    self.send_response(302)
                    self.send_header('Location', targets[self.path])
                    self.end_headers()
                else:
                    self.send_response(200)
                    self.send_header('Content-Type', 'text/html; charset=utf-8')
                    self.end_headers()
                    self.wfile.write(b'<p>TLS works</p>')
        cls.servers = []
        cls.threads = []
        for name, minimum, maximum in [('http', None, None), ('https', ssl.TLSVersion.TLSv1_2, ssl.TLSVersion.TLSv1_2),
                                        ('old', ssl.TLSVersion.TLSv1, ssl.TLSVersion.TLSv1)]:
            server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
            if minimum:
                context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
                context.minimum_version = minimum
                context.maximum_version = maximum
                if name == 'old':
                    context.set_ciphers('ALL:@SECLEVEL=0')
                context.load_cert_chain(cls.cert, key)
                server.socket = context.wrap_socket(server.socket, server_side=True)
            setattr(cls, name, server)
            cls.servers.append(server)
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            cls.threads.append(thread)

    @classmethod
    def tearDownClass(cls):
        for server in cls.servers:
            server.shutdown()
            server.server_close()
        for thread in cls.threads:
            thread.join()
        cls.folder.cleanup()

    def request(self, url, trust=True, old_control=False):
        return subprocess.run(['./transport', url, self.cert if trust else ''] + (['old-control'] if old_control else []),
                              capture_output=True, timeout=10).returncode

    def test_tls12_and_certificate_checks(self):
        url = f'https://localhost:{self.https.server_port}/verified'
        self.assertEqual(self.request(url), 0)
        self.assertIn(('/verified', 'TLSv1.2'), self.requests)
        self.assertEqual(self.request(url, trust=False), 1)
        self.assertEqual(self.request(f'https://127.0.0.1:{self.https.server_port}/wrong-host'), 1)

    def test_old_tls_is_rejected(self):
        url = f'https://localhost:{self.old.server_port}/old'
        self.assertEqual(self.request(url, old_control=True), 0)
        self.assertEqual(self.request(url), 1)

    def test_https_downgrade_is_rejected(self):
        self.requests.clear()
        self.assertEqual(self.request(f'https://localhost:{self.https.server_port}/downgrade'), 1)
        self.assertFalse(any(path == '/target' for path, _ in self.requests))

    def test_http_can_upgrade_to_https(self):
        self.assertEqual(self.request(f'http://localhost:{self.http.server_port}/upgrade'), 0)

    def test_non_web_redirect_and_redirect_loop_are_rejected(self):
        self.assertEqual(self.request(f'https://localhost:{self.https.server_port}/file'), 1)
        self.assertEqual(self.request(f'https://localhost:{self.https.server_port}/loop'), 1)


if __name__ == '__main__':
    unittest.main()
