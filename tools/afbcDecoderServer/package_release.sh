#!/usr/bin/env bash
# Build the YUView Wasm client and create a self-contained local release package.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_OUTPUT="$PROJECT_ROOT/build-wasm/YUViewApp"
RELEASE_DIR="$SCRIPT_DIR/Release"
DECODER_BINARY="$PROJECT_ROOT/afbcdec_linux_static"

if [[ ! -x "$PROJECT_ROOT/build-wasm.sh" ]]; then
  echo "Cannot find the project build script: $PROJECT_ROOT/build-wasm.sh" >&2
  exit 1
fi
if [[ ! -f "$DECODER_BINARY" ]]; then
  echo "Expected AFBC decoder file is missing: $DECODER_BINARY" >&2
  exit 1
fi

"$PROJECT_ROOT/build-wasm.sh"

required_files=(YUView.html YUView.js YUView.wasm qtloader.js qtlogo.svg)
for file in "${required_files[@]}"; do
  if [[ ! -f "$BUILD_OUTPUT/$file" ]]; then
    echo "Expected Wasm output is missing: $BUILD_OUTPUT/$file" >&2
    exit 1
  fi
done

rm -rf "$RELEASE_DIR"
mkdir -p "$RELEASE_DIR"

for file in "${required_files[@]}"; do
  install -m 0644 "$BUILD_OUTPUT/$file" "$RELEASE_DIR/$file"
done

# Emscripten may produce these files for other build configurations.
for optional_file in YUView.data YUView.worker.js YUView.wasm.map; do
  if [[ -f "$BUILD_OUTPUT/$optional_file" ]]; then
    install -m 0644 "$BUILD_OUTPUT/$optional_file" "$RELEASE_DIR/$optional_file"
  fi
done

# Web page favicon, served at /favicon.ico.
FAVICON_SOURCE="$PROJECT_ROOT/YUViewApp/images/YUView.ico"
if [[ -f "$FAVICON_SOURCE" ]]; then
  install -m 0644 "$FAVICON_SOURCE" "$RELEASE_DIR/favicon.ico"
else
  echo "Warning: favicon source not found: $FAVICON_SOURCE" >&2
fi

# Bundle a self-contained Python runtime (python-build-standalone) so the
# release runs on machines that have no suitable Python (e.g. an old
# Ubuntu 16.04 with only Python 3.5). These builds require glibc >= 2.17.
PYTHON_VERSION="${PYTHON_VERSION:-3.10.21}"
PYTHON_DIST="cpython-${PYTHON_VERSION}+20260901-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"
PYTHON_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20260901/${PYTHON_DIST}"
PYTHON_DIR="$RELEASE_DIR/python"

# Prefer a locally downloaded tarball so the packaging step works offline.
PYTHON_TARBALL="${PYTHON_TARBALL:-$HOME/Downloads/$PYTHON_DIST}"
if [[ -f "$PYTHON_TARBALL" ]]; then
  echo "Using local embedded Python tarball: $PYTHON_TARBALL"
else
  echo "Downloading embedded Python runtime: $PYTHON_DIST"
  PYTHON_TARBALL="/tmp/$PYTHON_DIST"
  if ! curl -fL --retry 3 -o "$PYTHON_TARBALL" "$PYTHON_URL"; then
    echo "Failed to download $PYTHON_URL" >&2
    exit 1
  fi
fi

mkdir -p "$PYTHON_DIR"
tar -xzf "$PYTHON_TARBALL" -C "$PYTHON_DIR" --strip-components=1
[[ "$PYTHON_TARBALL" == /tmp/* ]] && rm -f "$PYTHON_TARBALL"

if [[ ! -x "$PYTHON_DIR/bin/python3" ]]; then
  echo "Embedded Python runtime is broken: $PYTHON_DIR/bin/python3" >&2
  exit 1
fi
echo "Embedded Python runtime ready: $PYTHON_DIR/bin/python3"

install -m 0755 "$SCRIPT_DIR/afbc_decoder_server.py" "$RELEASE_DIR/afbc_decoder_server.py"
install -m 0755 "$DECODER_BINARY" "$RELEASE_DIR/afbcdec_linux_static"
# Optional self-signed certificate for HTTPS. The multithreaded Qt WASM build
# needs SharedArrayBuffer, which browsers only enable in a secure context. A
# plain-HTTP LAN IP is not a secure context, so LAN clients must use HTTPS.
# If openssl is available, generate a self-signed cert into the release so
# start.sh can serve HTTPS out of the box.
if command -v openssl >/dev/null 2>&1; then
  CERT_DIR="$RELEASE_DIR/certs"
  mkdir -p "$CERT_DIR"
  openssl req -x509 -newkey rsa:2048 -nodes \
    -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
    -days 3650 -subj "/CN=YUView" \
    -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" \
    >/dev/null 2>&1
  echo "Generated self-signed HTTPS certificate in $CERT_DIR/"
else
  echo "openssl not found; HTTPS self-signed cert will NOT be generated." >&2
fi

cat > "$RELEASE_DIR/start.sh" <<'EOF'
#!/usr/bin/env bash
# Start YUView and the AFBC decoder API from the same origin.
set -euo pipefail

RELEASE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export AFBC_STATIC_ROOT="$RELEASE_DIR"

# Serve HTTPS automatically when a cert/key pair is present in the release.
# LAN clients require a secure context for SharedArrayBuffer, so HTTPS is needed
# when accessing via a non-localhost IP. Override by setting AFBC_TLS_CERT and
# AFBC_TLS_KEY yourself.
if [[ -f "$RELEASE_DIR/certs/server.crt" && -f "$RELEASE_DIR/certs/server.key" ]]; then
  export AFBC_TLS_CERT="${AFBC_TLS_CERT:-$RELEASE_DIR/certs/server.crt}"
  export AFBC_TLS_KEY="${AFBC_TLS_KEY:-$RELEASE_DIR/certs/server.key}"
fi

# Prefer the bundled Python runtime; fall back to a system python3 (which must
# be new enough for the script, e.g. 3.10+) for development builds.
if [[ -x "$RELEASE_DIR/python/bin/python3" ]]; then
  exec "$RELEASE_DIR/python/bin/python3" "$RELEASE_DIR/afbc_decoder_server.py" "$@"
fi
exec python3 "$RELEASE_DIR/afbc_decoder_server.py" "$@"
EOF
chmod 0755 "$RELEASE_DIR/start.sh"

cat > "$RELEASE_DIR/README.txt" <<'EOF'
YUView WebAssembly release package
==================================

1. Start the combined Web UI and decoder service:

   ./start.sh --host 0.0.0.0 --port 8080

2. Open http://localhost:8080/ (local access) or https://<server-ip>:8080/.

The package includes a self-signed certificate, so start.sh serves HTTPS
automatically. LAN clients MUST use https:// (not http://) because browsers
only enable SharedArrayBuffer (required by the multithreaded Wasm build) in a
secure context; a plain-HTTP LAN IP is not a secure context.

When opening https://<server-ip>:8080/ for the first time, the browser will
warn about the self-signed certificate. Accept the warning (or install the
certificate as trusted) to proceed.

In YUView, open Settings -> RK Tools and set AFBC Decoder Service URL to
https://<server-ip>:8080 (or the corresponding URL).

The service emits COOP/COEP headers required by the multithreaded Wasm build.
For production, replace the self-signed cert with a proper one (set AFBC_TLS_CERT
and AFBC_TLS_KEY) and run this package under an unprivileged account.

The package bundles its own Python runtime in python/ (python-build-standalone,
glibc >= 2.17), so no Python installation is required on the target machine.
start.sh automatically uses it.
EOF

echo "Release package created: $RELEASE_DIR"
echo "Run: $RELEASE_DIR/start.sh --host 0.0.0.0 --port 8080"
