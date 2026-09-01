# AFBC decoder service

A small, dependency-free HTTP service for the WebAssembly build of YUView. It accepts **one AFBC frame** and returns the decoded raster bytes. The decoder binary remains on the internal server; clients cannot select programs, file paths, or extra command-line arguments.

## Prerequisites

- Python 3.8 or newer on Linux.
- A locally installed, executable AFBC decoder compatible with YUView's existing command line:

  ```text
  decoder -h <width>x<height>_<mode> -i input.afbc -o output.raw
  ```

## Run

The server is intentionally bound to loopback by default. Use a reverse proxy or an explicitly selected internal address to expose it on a LAN.

```bash
cd tools/afbcDecoderServer
export AFBC_DECODER_PATH=../../afbcdec_linux_static
export AFBC_ALLOWED_ORIGIN=https://yuview.intranet.example
python3 afbc_decoder_server.py
```

## Build a combined local release

Run the following from the repository root to build the Wasm client and create `Release/` next to this service:

```bash
./tools/afbcDecoderServer/package_release.sh
```

The package contains the browser assets, this service, a `start.sh` launcher, and a self-signed HTTPS certificate. It can serve the UI and `/v1/afbc/decode` from one origin, avoiding cross-origin configuration:

```bash
cd tools/afbcDecoderServer/Release
./start.sh --host 0.0.0.0 --port 8080
```

Open `https://<server-ip>:8080/`. `start.sh` serves HTTPS automatically because the package ships a self-signed certificate. The service adds the COOP and COEP headers required by Qt's multithreaded Wasm build.

> **HTTPS is required for LAN access.** Browsers only enable `SharedArrayBuffer` (required by the multithreaded Qt WASM build) in a **secure context**. `http://localhost` and `http://127.0.0.1` count as secure, but a plain-`http://` LAN IP does **not** — the COOP/COEP headers are ignored and the WASM app cannot start. Using `https://<server-ip>:8080/` makes the page a secure context so the app loads. On first visit the browser warns about the self-signed certificate; accept it, or install the cert as trusted.

The package bundles `afbcdec_linux_static`, which is the default decoder path. `AFBC_DECODER_PATH` remains available to override it. The YUView setting defaults to `http://127.0.0.1:8080`; change it to `https://<server-ip>:8080` when the browser connects to another host.

For production, replace the self-signed cert with a proper one by setting `AFBC_TLS_CERT` and `AFBC_TLS_KEY` before running `start.sh`.

Useful configuration variables:

| Variable | Default | Purpose |
| --- | ---: | --- |
| `AFBC_BIND_HOST` | `127.0.0.1` | Bind address. |
| `AFBC_PORT` | `8080` | Bind port. |
| `AFBC_DECODER_PATH` | `afbcdec_linux_static` beside the service | Override the bundled decoder path. |
| `AFBC_ALLOWED_ORIGIN` | unset | Optional single browser origin permitted by CORS. |
| `AFBC_STATIC_ROOT` | `Release/` beside the service | Directory exposed for `GET` requests. `GET /` serves `YUView.html`. |
| `AFBC_TLS_CERT` / `AFBC_TLS_KEY` | unset | Enable HTTPS when both are set (certificate and key file paths). |
| `AFBC_MAX_REQUEST_BYTES` | `67108864` | Maximum AFBC frame upload size. |
| `AFBC_MAX_WIDTH` / `AFBC_MAX_HEIGHT` | `8192` | Maximum frame dimensions. |
| `AFBC_MAX_PIXELS` | `33554432` | Maximum frame pixel count. |
| `AFBC_MAX_OUTPUT_BYTES` | `268435456` | Maximum decoded raster size. |
| `AFBC_DECODE_TIMEOUT_SECONDS` | `10` | Decoder process deadline. |
| `AFBC_MAX_CONCURRENT_DECODES` | `2` | Maximum simultaneous decoder processes. |

## API

`POST /v1/afbc/decode`

Required request headers:

```text
Content-Type: application/octet-stream
Content-Length: <compressed frame byte count>
X-Afbc-Width: 1920
X-Afbc-Height: 1080
X-Afbc-Mode: r8g8b8
```

`X-Afbc-Mode` is exactly the mode already produced by YUView: `r<rBits>g<gBits>b<bBits>[a<aBits>][_<yuvTf>_<splitMode>_<yoffset>_<layout>]`. Component bit depths are limited to `8`, `10`, `12`, `16`, `24`, or `32`; `yuvTf` and `splitMode` must be `0` or `1`; `yoffset` is `0` through `15`; and `layout` is `0` through `6`.

A successful response is `200 application/octet-stream`; its body is the raster data. Its length must be exactly:

$$width \times height \times \sum_{components}{\lceil bits / 8 \rceil}$$

Failures have a short JSON body such as `{"code":"decode_failed"}`. Decoder stderr, local file paths, and commands are intentionally not sent to clients.

## Safety properties

- Uses a configured fixed executable and `subprocess.Popen(..., shell=False)`.
- All decoder arguments are generated after strict header validation.
- Uses a unique temporary directory per request and deletes it on every exit path.
- Restricts compressed input, dimensions, pixel count, and expected raster output size.
- Enforces a decoder timeout and kills the decoder process group on timeout.
- Caps concurrent decoder processes.
- Verifies the decoder exit code and exact output size before returning bytes.

Run it under a dedicated unprivileged system account. Put HTTPS and internal-network access control in a reverse proxy if it is not loopback-only.

## Test

```bash
python3 -m unittest discover -s tests -v
```
