import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from afbc_decoder_server import AfbcDecoder, RequestError, parse_decode_request  # noqa: E402


class ParseDecodeRequestTests(unittest.TestCase):
    def setUp(self):
        self.config = SimpleNamespace(
            max_width=8192,
            max_height=8192,
            max_pixels=33_554_432,
            max_output_bytes=256 * 1024 * 1024,
        )

    def parse(self, mode="r8g8b8", width="1920", height="1080"):
        return parse_decode_request(
            {"X-Afbc-Width": width, "X-Afbc-Height": height, "X-Afbc-Mode": mode},
            self.config,
        )

    def test_rgb8_mode_accepted(self):
        request = self.parse(width="2", height="3")
        # The server no longer infers an exact output size: the decoder is the
        # authority on frame layout (e.g. AB30 is 4 bytes/pixel). expected=0
        # means "any size up to max_output_bytes"; the client validates the
        # actual frame size with its own bytesPerFrame().
        self.assertEqual(request.expected_output_size, 0)
        self.assertEqual((request.width, request.height), (2, 3))

    def test_rgba_with_custom_afbc_options(self):
        request = self.parse("r10g10b10a8_1_0_15_6", "2", "3")
        self.assertEqual(request.mode, "r10g10b10a8_1_0_15_6")
        self.assertEqual(request.expected_output_size, 0)

    def test_ab30_alpha2_mode_accepted(self):
        # AB30 = ABGR2101010 (2-bit alpha, 10-bit RGB) -> mode r10g10b10a2.
        request = self.parse("r10g10b10a2", "3840", "2160")
        self.assertEqual(request.mode, "r10g10b10a2")

    def test_rejects_shell_characters_in_mode(self):
        with self.assertRaises(RequestError) as context:
            self.parse("r8g8b8;touch /tmp/pwned")
        self.assertEqual(context.exception.code, "unsupported_mode")

    def test_rejects_excessive_pixel_count(self):
        with self.assertRaises(RequestError) as context:
            self.parse(width="8192", height="8192")
        self.assertEqual(context.exception.code, "invalid_parameters")

    def test_rejects_unsupported_component_depth(self):
        with self.assertRaises(RequestError):
            self.parse("r9g9b9")

    def test_decoder_diagnostic_is_limited_to_tail(self):
        with tempfile.TemporaryDirectory() as directory:
            log_path = Path(directory) / "decoder.log"
            log_path.write_bytes(b"x" * 10_000 + b"decoder failure")
            detail = AfbcDecoder._decoder_log_detail(log_path)
        self.assertEqual(detail, "x" * (8 * 1024 - len("decoder failure")) + "decoder failure")


if __name__ == "__main__":
    unittest.main()
