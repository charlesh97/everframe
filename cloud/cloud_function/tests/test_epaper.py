import io
import hashlib
import unittest

from PIL import Image

from epaper import (
    FRAME_BYTES,
    HEIGHT,
    PORTRAIT_HEIGHT,
    PORTRAIT_WIDTH,
    WIDTH,
    _atkinson_indexes,
    _pack_portrait_indexes,
    frame_preview_jpeg,
    frame_to_portrait,
    image_to_frame,
)


def png_bytes(image: Image.Image) -> bytes:
    output = io.BytesIO()
    image.save(output, "PNG")
    return output.getvalue()


class ImageToFrameTests(unittest.TestCase):
    def test_produces_exact_panel_size(self):
        source = Image.new("RGB", (100, 200), "white")
        frame = image_to_frame(png_bytes(source))
        self.assertEqual(len(frame), FRAME_BYTES)
        self.assertEqual(FRAME_BYTES, WIDTH * HEIGHT // 2)

    def test_uses_controller_nibble_codes(self):
        source = Image.new("RGB", (PORTRAIT_WIDTH, PORTRAIT_HEIGHT), "#7355a6")
        frame = image_to_frame(png_bytes(source))
        allowed = {0x0, 0x1, 0x2, 0x3, 0x5, 0x6}
        for packed in frame:
            self.assertIn(packed >> 4, allowed)
            self.assertIn(packed & 0x0F, allowed)

    def test_rotates_portrait_clockwise_into_native_framebuffer(self):
        indexes = bytearray([3] * (PORTRAIT_WIDTH * PORTRAIT_HEIGHT))
        indexes[PORTRAIT_WIDTH * PORTRAIT_HEIGHT // 2 :] = bytes([4]) * (
            PORTRAIT_WIDTH * PORTRAIT_HEIGHT // 2
        )
        frame = _pack_portrait_indexes(indexes)
        first_row = frame[: WIDTH // 2]

        # Logical portrait bottom becomes native left; top becomes native right.
        self.assertEqual(set(first_row[: WIDTH // 4]), {0x55})
        self.assertEqual(set(first_row[WIDTH // 4 :]), {0x33})

    def test_serpentine_atkinson_has_stable_small_fixture(self):
        source = Image.new("RGB", (8, 8))
        source.putdata(
            [
                (x * 31, y * 31, ((x + y) * 17) % 256)
                for y in range(8)
                for x in range(8)
            ]
        )
        indexes = _atkinson_indexes(source)
        self.assertEqual(
            hashlib.sha256(indexes).hexdigest(),
            "3e0f020dec93572b50d15434338e944405992e6655ade19647dcc6c6e1754db5",
        )

    def test_decodes_exact_frame_back_to_portrait_preview(self):
        source = Image.new("RGB", (PORTRAIT_WIDTH, PORTRAIT_HEIGHT), "red")
        source.paste("blue", (0, PORTRAIT_HEIGHT // 2, PORTRAIT_WIDTH, PORTRAIT_HEIGHT))

        frame = image_to_frame(png_bytes(source))
        preview = frame_to_portrait(frame)

        self.assertEqual(preview.size, (PORTRAIT_WIDTH, PORTRAIT_HEIGHT))
        self.assertEqual(preview.getpixel((0, 0)), (113, 24, 19))
        self.assertEqual(preview.getpixel((0, PORTRAIT_HEIGHT - 1)), (36, 70, 139))

        with Image.open(io.BytesIO(frame_preview_jpeg(frame))) as jpeg:
            self.assertEqual(jpeg.format, "JPEG")
            self.assertEqual(jpeg.size, (PORTRAIT_WIDTH, PORTRAIT_HEIGHT))

    def test_rejects_invalid_packed_frame(self):
        with self.assertRaisesRegex(ValueError, "exactly 192000 bytes"):
            frame_to_portrait(b"")

        with self.assertRaisesRegex(ValueError, "unsupported panel color code"):
            frame_to_portrait(bytes([0x44]) + bytes(FRAME_BYTES - 1))

    def test_rejects_non_image_input(self):
        with self.assertRaises(Exception):
            image_to_frame(b"not an image")


if __name__ == "__main__":
    unittest.main()
