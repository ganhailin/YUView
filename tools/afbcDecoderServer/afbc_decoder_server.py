#!/usr/bin/env python3
"""Small internal HTTP service that decodes one AFBC frame per request.

The service deliberately has no third-party dependencies.  It invokes only the
decoder selected by its administrator, and never accepts executable paths or
arbitrary command-line arguments from a client.
"""

from __future__ import annotations

import argparse
import json
import logging
import mimetypes
import os
import re
import signal
import shutil
import socket
import ssl
import subprocess
import tempfile
import threading
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Final
from urllib.parse import unquote, urlsplit


LOG = logging.getLogger("afbc-decoder-server")
MODE_PATTERN: Final = re.compile(
	r"^r(?P<red>8|10|12|16|24|32)g(?P<green>8|10|12|16|24|32)"
	r"b(?P<blue>8|10|12|16|24|32)"
	r"(?:a(?P<alpha>8|10|12|16|24|32))?"
	r"(?:_(?P<yuv_tf>[01])_(?P<split_mode>[01])_(?P<yoffset>(?:[0-9]|1[0-5]))"
	r"_(?P<layout>[0-6]))?$"
)
STATIC_RELEASE_FILES: Final = frozenset(
	{
		"YUView.html",
		"YUView.js",
		"YUView.wasm",
		"YUView.data",
		"YUView.worker.js",
		"YUView.wasm.map",
		"qtloader.js",
		"qtlogo.svg",
		"favicon.ico",
	}
)
MAX_DECODER_LOG_BYTES: Final = 8 * 1024


class RequestError(Exception):
	"""An expected client-facing request error."""

	def __init__(self, status: HTTPStatus, code: str, detail: str = "") -> None:
		self.status = status
		self.code = code
		self.detail = detail
		super().__init__(code)


@dataclass(frozen=True)
class DecodeRequest:
	width: int
	height: int
	mode: str
	expected_output_size: int


@dataclass(frozen=True)
class Config:
	decoder_path: Path
	max_request_bytes: int
	max_width: int
	max_height: int
	max_pixels: int
	max_output_bytes: int
	timeout_seconds: float
	max_concurrent_decodes: int
	allowed_origin: str | None
	static_root: Path
	tls_cert: str | None
	tls_key: str | None

	@property
	def tls_enabled(self) -> bool:
		return bool(self.tls_cert and self.tls_key)

	@classmethod
	def from_environment(cls) -> "Config":
		# Release/start.sh places the trusted decoder beside this script. An
		# administrator can still override it for development or upgrades.
		decoder_value = os.environ.get(
			"AFBC_DECODER_PATH", Path(__file__).resolve().parent / "afbcdec_linux_static"
		)

		decoder_path = Path(decoder_value).resolve(strict=True)
		if not decoder_path.is_file() or not os.access(decoder_path, os.X_OK):
			raise ValueError("AFBC_DECODER_PATH must be an executable regular file")

		def positive_int(name: str, default: int) -> int:
			value = int(os.environ.get(name, default))
			if value <= 0:
				raise ValueError(f"{name} must be positive")
			return value

		timeout_seconds = float(os.environ.get("AFBC_DECODE_TIMEOUT_SECONDS", "10"))
		if timeout_seconds <= 0:
			raise ValueError("AFBC_DECODE_TIMEOUT_SECONDS must be positive")

		static_root = Path(
			os.environ.get("AFBC_STATIC_ROOT", Path(__file__).resolve().parent / "Release")
		).resolve()

		# Optional TLS. When both cert and key are set, the server serves HTTPS.
		# This is required for LAN clients: plain-HTTP over a non-localhost IP is not a
		# secure context, so browsers disable SharedArrayBuffer and the multithreaded
		# Qt WASM build cannot start.
		tls_cert = os.environ.get("AFBC_TLS_CERT") or None
		tls_key = os.environ.get("AFBC_TLS_KEY") or None
		if bool(tls_cert) != bool(tls_key):
			raise ValueError("AFBC_TLS_CERT and AFBC_TLS_KEY must be set together")
		if tls_cert:
			tls_cert_path = Path(tls_cert).resolve(strict=True)
			tls_key_path = Path(tls_key).resolve(strict=True)
			if not tls_cert_path.is_file() or not tls_key_path.is_file():
				raise ValueError("AFBC_TLS_CERT / AFBC_TLS_KEY must point to existing files")
			tls_cert, tls_key = str(tls_cert_path), str(tls_key_path)

		return cls(
			decoder_path=decoder_path,
			max_request_bytes=positive_int("AFBC_MAX_REQUEST_BYTES", 64 * 1024 * 1024),
			max_width=positive_int("AFBC_MAX_WIDTH", 8192),
			max_height=positive_int("AFBC_MAX_HEIGHT", 8192),
			max_pixels=positive_int("AFBC_MAX_PIXELS", 33_554_432),
			max_output_bytes=positive_int("AFBC_MAX_OUTPUT_BYTES", 256 * 1024 * 1024),
			timeout_seconds=timeout_seconds,
			max_concurrent_decodes=positive_int("AFBC_MAX_CONCURRENT_DECODES", 2),
			allowed_origin=os.environ.get("AFBC_ALLOWED_ORIGIN") or None,
			static_root=static_root,
			tls_cert=tls_cert,
			tls_key=tls_key,
		)


def parse_positive_header(headers, name: str, maximum: int) -> int:
	raw_value = headers.get(name)
	if raw_value is None or not raw_value.isascii() or not raw_value.isdecimal():
		raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_parameters")
	value = int(raw_value)
	if value <= 0 or value > maximum:
		raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_parameters")
	return value


def is_relative_to(path: Path, other: Path) -> bool:
	"""Return whether *path* is equal to or under *other*.

	`Path.is_relative_to()` is only available on Python 3.9+, so this small
	helper keeps the service compatible with Python 3.8.
	"""
	try:
		path.relative_to(other)
	except ValueError:
		return False
	return True


def parse_decode_request(headers, config: Config) -> DecodeRequest:
	width = parse_positive_header(headers, "X-Afbc-Width", config.max_width)
	height = parse_positive_header(headers, "X-Afbc-Height", config.max_height)
	if width * height > config.max_pixels:
		raise RequestError(HTTPStatus.BAD_REQUEST, "invalid_parameters")

	mode = headers.get("X-Afbc-Mode", "")
	match = MODE_PATTERN.fullmatch(mode)
	if not match:
		raise RequestError(HTTPStatus.BAD_REQUEST, "unsupported_mode")

	# This mirrors PixelFormatRGB::bytesPerFrame for the formats accepted by
	# the command-line decoder: channels use whole-byte storage.
	component_bits = [int(match.group("red")), int(match.group("green")), int(match.group("blue"))]
	if match.group("alpha") is not None:
		component_bits.append(int(match.group("alpha")))
	bytes_per_pixel = sum((bits + 7) // 8 for bits in component_bits)
	expected_output_size = width * height * bytes_per_pixel
	if expected_output_size > config.max_output_bytes:
		raise RequestError(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "output_too_large")

	return DecodeRequest(width, height, mode, expected_output_size)


class AfbcDecoder:
	def __init__(self, config: Config) -> None:
		self.config = config
		self.slots = threading.BoundedSemaphore(config.max_concurrent_decodes)

	def decode(self, request: DecodeRequest, payload: bytes) -> bytes:
		if not self.slots.acquire(blocking=False):
			raise RequestError(HTTPStatus.SERVICE_UNAVAILABLE, "decoder_busy")

		try:
			return self._decode_with_fixed_command(request, payload)
		finally:
			self.slots.release()

	def _decode_with_fixed_command(self, request: DecodeRequest, payload: bytes) -> bytes:
		with tempfile.TemporaryDirectory(prefix="afbc-decode-") as temporary_directory:
			directory = Path(temporary_directory)
			input_path = directory / "input.afbc"
			output_path = directory / "output.raw"
			decoder_stdout_path = directory / "decoder.stdout.log"
			decoder_stderr_path = directory / "decoder.stderr.log"
			input_path.write_bytes(payload)

			# # Diagnostic: log payload size and a few hashes to detect corruption.
			# import hashlib
			# LOG.info("DECODE_PAYLOAD size=%d md5=%s first16=%s last16=%s",
			# 		 len(payload), hashlib.md5(payload).hexdigest(),
			# 		 payload[:16].hex(), payload[-16:].hex())
			# # Write a copy of the received payload for offline comparison.
			# try:
			# 	(Path("/tmp/afbc_decode_payload.afbc")).write_bytes(payload)
			# 	LOG.info("DECODE_PAYLOAD copied to /tmp/afbc_decode_payload.afbc")
			# except OSError:
			# 	pass

			# Every value below is generated by this service after strict
			# validation. shell=False is intentional and must not be changed.
			command = [
				str(self.config.decoder_path),
				"-h",
				f"{request.width}x{request.height}_{request.mode}",
				"-i",
				input_path.name,
				"-o",
				output_path.name,
			]
			process: subprocess.Popen[bytes] | None = None
			try:
				# Temporary files avoid unbounded output pipes if a broken
				# decoder prints excessive diagnostics. Bounded tails are
				# logged and returned to the client only on failure.
				# NOTE: a single parenthesized `with (...)` is Python 3.10+
				# only, so split it to stay compatible with Python 3.8.
				with decoder_stdout_path.open("wb") as decoder_stdout:
					with decoder_stderr_path.open("wb") as decoder_stderr:
						process = subprocess.Popen(
							command,
							cwd=directory,
							stdin=subprocess.DEVNULL,
							stdout=decoder_stdout,
							stderr=decoder_stderr,
							shell=False,
							start_new_session=True,
						)
						process.wait(timeout=self.config.timeout_seconds)
			except subprocess.TimeoutExpired:
				if process is not None:
					os.killpg(process.pid, signal.SIGKILL)
					process.wait()
				stdout, stderr = self._decoder_log_details(decoder_stdout_path, decoder_stderr_path)
				self._log_decoder_failure("timed out", stdout, stderr)
				raise RequestError(HTTPStatus.GATEWAY_TIMEOUT,
								   "decode_timeout",
								   self._format_decoder_details(stdout, stderr)) from None
			except OSError:
				LOG.exception("Could not start configured AFBC decoder")
				raise RequestError(HTTPStatus.SERVICE_UNAVAILABLE, "decoder_unavailable") from None

			if process.returncode != 0:
				stdout, stderr = self._decoder_log_details(decoder_stdout_path, decoder_stderr_path)
				self._log_decoder_failure(f"exited with status {process.returncode}", stdout, stderr)
				raise RequestError(
					HTTPStatus.UNPROCESSABLE_ENTITY,
					"decode_failed",
					self._format_decoder_details(stdout, stderr),
				)

			try:
				output_size = output_path.stat().st_size
			except FileNotFoundError:
				raise RequestError(HTTPStatus.UNPROCESSABLE_ENTITY, "decode_failed") from None
			if output_size != request.expected_output_size:
				LOG.warning("Decoder output had unexpected size %d (expected %d)", output_size,
							request.expected_output_size)
				stdout, stderr = self._decoder_log_details(decoder_stdout_path, decoder_stderr_path)
				self._log_decoder_failure("produced an unexpected output size", stdout, stderr)
				raise RequestError(
					HTTPStatus.UNPROCESSABLE_ENTITY,
					"invalid_decoder_output",
					self._format_decoder_details(stdout, stderr),
				)

			return output_path.read_bytes()

	@staticmethod
	def _decoder_log_detail(decoder_log_path: Path) -> str:
		try:
			with decoder_log_path.open("rb") as decoder_log:
				decoder_log.seek(0, os.SEEK_END)
				decoder_log.seek(max(0, decoder_log.tell() - MAX_DECODER_LOG_BYTES))
				detail = decoder_log.read(MAX_DECODER_LOG_BYTES).decode("utf-8", errors="replace").strip()
		except OSError:
			return ""
		return detail

	@classmethod
	def _decoder_log_details(cls, stdout_path: Path, stderr_path: Path) -> tuple[str, str]:
		return cls._decoder_log_detail(stdout_path), cls._decoder_log_detail(stderr_path)

	@staticmethod
	def _format_decoder_details(stdout: str, stderr: str) -> str:
		if not stdout and not stderr:
			return "Decoder failed without diagnostic output."
		return f"stdout:\n{stdout or '(empty)'}\n\nstderr:\n{stderr or '(empty)'}"

	@staticmethod
	def _log_decoder_failure(reason: str, stdout: str, stderr: str) -> None:
		LOG.warning("AFBC decoder %s. stdout:\n%s\nstderr:\n%s",
					reason,
					stdout or "(empty)",
					stderr or "(empty)")


class DecoderHTTPServer(ThreadingHTTPServer):
	daemon_threads = True

	def __init__(self, address: tuple[str, int], config: Config) -> None:
		self.config = config
		self.tls_context: ssl.SSLContext | None = None
		if config.tls_enabled:
			self.tls_context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
			self.tls_context.load_cert_chain(certfile=config.tls_cert, keyfile=config.tls_key)
		super().__init__(address, DecoderRequestHandler)
		self.decoder = AfbcDecoder(config)

	def get_request(self):
		"""Peek the first byte to decide whether this is TLS or plain HTTP.

		The Qt WASM app must be served over HTTPS for LAN clients (secure context
		is required for SharedArrayBuffer), but users may still type http://. To
		avoid TLS handshake failures and garbled log output, detect the protocol
		and let plain-HTTP requests get a friendly redirect/notice instead.
		"""
		raw_socket, address = super().get_request()
		if self.tls_context is None:
			return raw_socket, address
		raw_socket.settimeout(5)
		try:
			# 0x16 is the first byte of a TLS handshake record.
			first_byte = raw_socket.recv(1, socket.MSG_PEEK)
		except (socket.timeout, OSError):
			first_byte = b""
		raw_socket.settimeout(None)
		if first_byte == b"\x16":
			# TLS client (https://). Wrap the socket; the peeked byte stays in the
			# socket buffer so the handshake reads it normally.
			tls_socket = self.tls_context.wrap_socket(raw_socket, server_side=True)
			return tls_socket, address
		# Plain HTTP client (http://). Leave the socket unwrapped.
		return raw_socket, address


class DecoderRequestHandler(BaseHTTPRequestHandler):
	protocol_version = "HTTP/1.1"
	server: DecoderHTTPServer

	def do_OPTIONS(self) -> None:  # noqa: N802
		if self.path != "/v1/afbc/decode" or not self._origin_is_allowed():
			self._send_error(HTTPStatus.NOT_FOUND, "not_found")
			return
		self.send_response(HTTPStatus.NO_CONTENT)
		self._send_cors_headers()
		self.send_header("Access-Control-Allow-Methods", "POST, OPTIONS")
		self.send_header("Access-Control-Allow-Headers", "Content-Type, X-Afbc-Width, X-Afbc-Height, X-Afbc-Mode")
		self.send_header("Access-Control-Max-Age", "600")
		self.send_header("Content-Length", "0")
		self.end_headers()

	def do_POST(self) -> None:  # noqa: N802
		if self.path != "/v1/afbc/decode":
			self._send_error(HTTPStatus.NOT_FOUND, "not_found")
			return
		if not self._require_https():
			return
		if not self._origin_is_allowed():
			self._send_error(HTTPStatus.FORBIDDEN, "origin_not_allowed")
			return

		try:
			content_length = self._content_length()
			request = parse_decode_request(self.headers, self.server.config)
			payload = self._read_payload(content_length)
			result = self.server.decoder.decode(request, payload)
		except RequestError as error:
			self._send_error(error.status, error.code, error.detail)
			return

		self.send_response(HTTPStatus.OK)
		self._send_cors_headers()
		self.send_header("Content-Type", "application/octet-stream")
		self.send_header("Content-Length", str(len(result)))
		self.send_header("Cache-Control", "no-store")
		self.send_header("X-Content-Type-Options", "nosniff")
		self.end_headers()
		self.wfile.write(result)

	def do_GET(self) -> None:  # noqa: N802
		if self.path == "/healthz":
			self._send_health_response()
			return
		if not self._require_https():
			return

		requested_path = urlsplit(self.path).path
		if requested_path == "/":
			requested_path = "/YUView.html"

		try:
			file_path = self._static_file_path(requested_path)
		except RequestError as error:
			self._send_error(error.status, error.code, error.detail)
			return

		try:
			content_length = file_path.stat().st_size
			content_type = mimetypes.guess_type(file_path.name)[0] or "application/octet-stream"
			if file_path.suffix == ".wasm":
				content_type = "application/wasm"
			elif file_path.suffix == ".js":
				content_type = "application/javascript"
			with file_path.open("rb") as static_file:
				self.send_response(HTTPStatus.OK)
				self._send_static_security_headers()
				self.send_header("Content-Type", content_type)
				self.send_header("Content-Length", str(content_length))
				self.send_header("Cache-Control", "no-store")
				self.send_header("X-Content-Type-Options", "nosniff")
				self.end_headers()
				shutil.copyfileobj(static_file, self.wfile)
		except OSError:
			LOG.exception("Could not serve static release file %s", file_path)
			self._send_error(HTTPStatus.INTERNAL_SERVER_ERROR, "static_file_error")

	def _static_file_path(self, requested_path: str) -> Path:
		if not requested_path.startswith("/"):
			raise RequestError(HTTPStatus.NOT_FOUND, "not_found")

		relative_path = Path(unquote(requested_path).lstrip("/"))
		if (relative_path.is_absolute() or ".." in relative_path.parts or
			len(relative_path.parts) != 1 or relative_path.name not in STATIC_RELEASE_FILES):
			raise RequestError(HTTPStatus.NOT_FOUND, "not_found")

		static_root = self.server.config.static_root
		candidate = (static_root / relative_path).resolve()
		if not is_relative_to(candidate, static_root) or not candidate.is_file():
			raise RequestError(HTTPStatus.NOT_FOUND, "not_found")
		return candidate

	def _send_health_response(self) -> None:
		body = b'{"status":"ok"}'
		self.send_response(HTTPStatus.OK)
		self._send_static_security_headers()
		self.send_header("Content-Type", "application/json")
		self.send_header("Content-Length", str(len(body)))
		self.send_header("Cache-Control", "no-store")
		self.end_headers()
		self.wfile.write(body)

	def _content_length(self) -> int:
		value = self.headers.get("Content-Length")
		if value is None or not value.isascii() or not value.isdecimal():
			raise RequestError(HTTPStatus.LENGTH_REQUIRED, "content_length_required")
		length = int(value)
		if length <= 0:
			raise RequestError(HTTPStatus.BAD_REQUEST, "empty_input")
		if length > self.server.config.max_request_bytes:
			raise RequestError(HTTPStatus.REQUEST_ENTITY_TOO_LARGE, "input_too_large")
		return length

	def _read_payload(self, content_length: int) -> bytes:
		payload = self.rfile.read(content_length)
		if len(payload) != content_length:
			raise RequestError(HTTPStatus.BAD_REQUEST, "truncated_input")
		return payload

	def _origin_is_allowed(self) -> bool:
		allowed_origin = self.server.config.allowed_origin
		return allowed_origin is None or self.headers.get("Origin") in (None, allowed_origin)

	def _send_cors_headers(self) -> None:
		allowed_origin = self.server.config.allowed_origin
		if allowed_origin and self.headers.get("Origin") == allowed_origin:
			self.send_header("Access-Control-Allow-Origin", allowed_origin)
			self.send_header("Vary", "Origin")

	def _is_tls(self) -> bool:
		return isinstance(self.connection, ssl.SSLSocket)

	def _require_https(self) -> bool:
		"""When TLS is enabled, the Qt WASM app needs a secure context, so plain
		HTTP must not serve the app or the decode API. Return True if the request
		is allowed, False if the handler should stop (already responded)."""
		if not self.server.config.tls_enabled or self._is_tls():
			return True
		# Plain HTTP against a TLS-enabled server.
		if self.command == "POST":
			self._send_error(HTTPStatus.UPGRADE_REQUIRED, "https_required",
							 "This service requires HTTPS. Use https:// instead of http://.")
			return False
		host = self.headers.get("Host") or self.server.server_address[0]
		path = urlsplit(self.path).path or "/"
		target = f"https://{host}{path}"
		body = (f"<html><body><h3>YUView requires HTTPS</h3>"
				f"<p>SharedArrayBuffer (needed by the multithreaded WASM build) is only available "
				f"in a secure context. Please open:</p>"
				f"<p><a href='{target}'>{target}</a></p>"
				f"<p>If the browser warns about a self-signed certificate, accept it to proceed.</p>"
				f"</body></html>").encode("utf-8")
		self.send_response(HTTPStatus.OK)
		self.send_header("Content-Type", "text/html; charset=utf-8")
		self.send_header("Content-Length", str(len(body)))
		self.send_header("Cache-Control", "no-store")
		self.end_headers()
		self.wfile.write(body)
		return False

	def _send_static_security_headers(self) -> None:
		# Qt's multithreaded Wasm build requires cross-origin isolation to
		# enable SharedArrayBuffer. These apply to every static release asset.
		self.send_header("Cross-Origin-Opener-Policy", "same-origin")
		self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
		self.send_header("Cross-Origin-Resource-Policy", "same-origin")

	def _send_error(self, status: HTTPStatus, code: str, detail: str = "") -> None:
		body_data = {"code": code}
		if detail:
			body_data["detail"] = detail
		body = json.dumps(body_data, separators=(",", ":")).encode("utf-8")
		self.send_response(status)
		self._send_cors_headers()
		self.send_header("Content-Type", "application/json")
		self.send_header("Content-Length", str(len(body)))
		self.send_header("Cache-Control", "no-store")
		self.end_headers()
		self.wfile.write(body)

	def log_message(self, format: str, *args) -> None:
		LOG.info("%s - %s", self.client_address[0], format % args)


def main() -> None:
	parser = argparse.ArgumentParser(description=__doc__)
	parser.add_argument("--host", default=os.environ.get("AFBC_BIND_HOST", "127.0.0.1"))
	parser.add_argument("--port", type=int, default=int(os.environ.get("AFBC_PORT", "8080")))
	arguments = parser.parse_args()
	if not 1 <= arguments.port <= 65535:
		parser.error("--port must be between 1 and 65535")

	try:
		config = Config.from_environment()
	except (OSError, ValueError) as error:
		parser.error(str(error))

	logging.basicConfig(level=os.environ.get("AFBC_LOG_LEVEL", "INFO").upper(),
						format="%(asctime)s %(levelname)s %(name)s: %(message)s")
	with DecoderHTTPServer((arguments.host, arguments.port), config) as server:
		# Protocol detection is done per-connection in get_request(), so the same
		# port serves HTTPS (for LAN/secure context) and gives a friendly notice
		# on plain HTTP instead of a TLS handshake failure.
		scheme = "https" if config.tls_enabled else "http"
		LOG.info("Listening on %s://%s:%d", scheme, arguments.host, arguments.port)
		server.serve_forever()


if __name__ == "__main__":
	main()
