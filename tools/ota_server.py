import os
import sys
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer


class Handler(SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "ota"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8899
    os.chdir(root)
    ThreadingHTTPServer(("0.0.0.0", port), Handler).serve_forever()


if __name__ == "__main__":
    main()
