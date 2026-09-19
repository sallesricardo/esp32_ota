#!/usr/bin/env python3

import http.server
import ssl
import os
import sys

PORT = 8070
DIRECTORY = "./firmware"


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

    def log_message(self, format, *args):
        # Log colorido para facilitar visualização
        sys.stderr.write("\033[1;36m[%s]\033[0m %s\n" % (self.log_date_time_string(), format % args))


def run_server():
    httpd = http.server.HTTPServer(("0.0.0.0", PORT), Handler)

    # Configura SSL
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile="server_cert.pem", keyfile="server_key.pem")
    httpd.socket = context.wrap_socket(httpd.socket, server_side=True)

    print(f"\033[1;32m✓ Servidor HTTPS rodando em https://0.0.0.0:{PORT}\033[0m")
    print(f"\033[1;33m✓ Servindo arquivos de: {os.path.abspath(DIRECTORY)}\033[0m")
    print(f"\033[1;34m✓ Certificado: server_cert.pem\033[0m")
    print(f"\033[1;31m⚠ Pressione Ctrl+C para parar\033[0m\n")

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n\033[1;33mServidor parado.\033[0m")
        httpd.server_close()


if __name__ == "__main__":
    run_server()
