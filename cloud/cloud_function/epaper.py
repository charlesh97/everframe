"""Conversion to the Waveshare 7.3-inch E panel's native frame format."""

from __future__ import annotations

import io
import math

from PIL import Image, ImageOps

WIDTH = 800
HEIGHT = 480
PORTRAIT_WIDTH = 480
PORTRAIT_HEIGHT = 800
FRAME_BYTES = WIDTH * HEIGHT // 2

# Canonical source color, measured physical ink color, and controller nibble.
# Measurements are OpenDisplay's SPECTRA_7_3_6COLOR_V2 palette (2026-03-15).
PANEL_COLORS = (
    ((0, 0, 0),       (31, 24, 41),   0x0),  # black
    ((255, 255, 255), (168, 180, 182), 0x1),  # white
    ((255, 255, 0),   (180, 173, 0),   0x2),  # yellow
    ((255, 0, 0),     (113, 24, 19),   0x3),  # red
    ((0, 0, 255),     (36, 70, 139),   0x5),  # blue
    ((0, 255, 0),     (50, 84, 60),    0x6),  # green
)

_WAB = 1.5
_LUMINANCE = (0.2126729, 0.7151522, 0.0721750)
_ATKINSON = (
    (1, 0, 1.0 / 8.0),
    (2, 0, 1.0 / 8.0),
    (-1, 1, 1.0 / 8.0),
    (0, 1, 1.0 / 8.0),
    (1, 1, 1.0 / 8.0),
    (0, 2, 1.0 / 8.0),
)


def _srgb_to_linear(value: int) -> float:
    channel = value / 255.0
    return channel / 12.92 if channel <= 0.04045 else ((channel + 0.055) / 1.055) ** 2.4


def _linear_to_srgb(value: float) -> int:
    channel = min(1.0, max(0.0, value))
    encoded = 12.92 * channel if channel <= 0.0031308 else 1.055 * channel ** (1.0 / 2.4) - 0.055
    return int(encoded * 255.0 + 0.5)


_SRGB_TO_LINEAR = tuple(_srgb_to_linear(value) for value in range(256))


def _linear_rgb_to_oklab(red: float, green: float, blue: float) -> tuple[float, float, float]:
    l_value = 0.4122214708 * red + 0.5363325363 * green + 0.0514459929 * blue
    m_value = 0.2119034982 * red + 0.6806995451 * green + 0.1073969566 * blue
    s_value = 0.0883024619 * red + 0.2817188376 * green + 0.6299787005 * blue
    l_root = l_value ** (1.0 / 3.0)
    m_root = m_value ** (1.0 / 3.0)
    s_root = s_value ** (1.0 / 3.0)
    return (
        0.2104542553 * l_root + 0.7936177850 * m_root - 0.0040720468 * s_root,
        1.9779984951 * l_root - 2.4285922050 * m_root + 0.4505937099 * s_root,
        0.0259040371 * l_root + 0.7827717662 * m_root - 0.8086757660 * s_root,
    )


def _rgb_to_oklab(red: int, green: int, blue: int) -> tuple[float, float, float]:
    return _linear_rgb_to_oklab(
        _SRGB_TO_LINEAR[red], _SRGB_TO_LINEAR[green], _SRGB_TO_LINEAR[blue]
    )


_MEASURED_RGB = tuple(measured for _, measured, _ in PANEL_COLORS)
_MEASURED_LAB = tuple(_rgb_to_oklab(*color) for color in _MEASURED_RGB)


def _luminance(rgb: tuple[float, float, float]) -> float:
    return sum(channel * weight for channel, weight in zip(rgb, _LUMINANCE))


def _tone_compress(image: Image.Image) -> bytearray:
    """Map source luminance into the measured black-to-white panel range."""
    black_linear = tuple(_SRGB_TO_LINEAR[channel] for channel in _MEASURED_RGB[0])
    white_linear = tuple(_SRGB_TO_LINEAR[channel] for channel in _MEASURED_RGB[1])
    black_y = _luminance(black_linear)
    white_y = _luminance(white_linear)
    display_range = white_y - black_y
    output = bytearray(image.width * image.height * 3)

    for pixel, (red, green, blue) in enumerate(image.getdata()):
        linear = (
            _SRGB_TO_LINEAR[red],
            _SRGB_TO_LINEAR[green],
            _SRGB_TO_LINEAR[blue],
        )
        source_y = _luminance(linear)
        target_y = black_y + source_y * display_range
        if source_y > 1e-8:
            scale = target_y / source_y
            mapped = tuple(_linear_to_srgb(channel * scale) for channel in linear)
        else:
            neutral = _linear_to_srgb(black_y)
            mapped = (neutral, neutral, neutral)
        offset = pixel * 3
        output[offset : offset + 3] = bytes(mapped)
    return output


def _closest_ink(red: float, green: float, blue: float) -> int:
    lab = _linear_rgb_to_oklab(
        _SRGB_TO_LINEAR[min(255, max(0, int(red + 0.5)))],
        _SRGB_TO_LINEAR[min(255, max(0, int(green + 0.5)))],
        _SRGB_TO_LINEAR[min(255, max(0, int(blue + 0.5)))],
    )
    best_index = 0
    best_distance = math.inf
    for index, palette_lab in enumerate(_MEASURED_LAB):
        delta_l = lab[0] - palette_lab[0]
        delta_a = _WAB * (lab[1] - palette_lab[1])
        delta_b = _WAB * (lab[2] - palette_lab[2])
        distance = delta_l * delta_l + delta_a * delta_a + delta_b * delta_b
        if distance < best_distance:
            best_index = index
            best_distance = distance
    return best_index


def _atkinson_indexes(image: Image.Image) -> bytearray:
    """Return measured-palette indexes using serpentine Atkinson diffusion."""
    width, height = image.size
    work = list(_tone_compress(image))
    output = bytearray(width * height)

    for y_position in range(height):
        reverse = y_position % 2 == 1
        x_positions = range(width - 1, -1, -1) if reverse else range(width)
        for x_position in x_positions:
            pixel = y_position * width + x_position
            offset = pixel * 3
            red = min(255.0, max(0.0, work[offset]))
            green = min(255.0, max(0.0, work[offset + 1]))
            blue = min(255.0, max(0.0, work[offset + 2]))
            ink = _closest_ink(red, green, blue)
            output[pixel] = ink
            measured = _MEASURED_RGB[ink]
            errors = (red - measured[0], green - measured[1], blue - measured[2])

            for delta_x, delta_y, weight in _ATKINSON:
                neighbor_x = x_position + (-delta_x if reverse else delta_x)
                neighbor_y = y_position + delta_y
                if 0 <= neighbor_x < width and neighbor_y < height:
                    neighbor = (neighbor_y * width + neighbor_x) * 3
                    work[neighbor] += errors[0] * weight
                    work[neighbor + 1] += errors[1] * weight
                    work[neighbor + 2] += errors[2] * weight
    return output


def _pack_portrait_indexes(indexes: bytes) -> bytes:
    """Rotate 480x800 portrait palette indexes and pack controller nibbles."""
    if len(indexes) != PORTRAIT_WIDTH * PORTRAIT_HEIGHT:
        raise ValueError("portrait index buffer has the wrong size")
    codes = tuple(code for _, _, code in PANEL_COLORS)
    native = bytearray(WIDTH * HEIGHT)
    for portrait_x in range(PORTRAIT_WIDTH):
        native_row = portrait_x * WIDTH
        for portrait_y in range(PORTRAIT_HEIGHT):
            native[native_row + WIDTH - 1 - portrait_y] = indexes[
                portrait_y * PORTRAIT_WIDTH + portrait_x
            ]

    output = bytearray(FRAME_BYTES)
    for offset in range(0, len(native), 2):
        output[offset // 2] = (codes[native[offset]] << 4) | codes[native[offset + 1]]
    return bytes(output)


def image_to_frame(image_bytes: bytes) -> bytes:
    """Fit portrait art and encode it with measured-palette Atkinson dithering."""
    with Image.open(io.BytesIO(image_bytes)) as opened:
        rgb = ImageOps.exif_transpose(opened).convert("RGB")
        portrait = ImageOps.fit(
            rgb, (PORTRAIT_WIDTH, PORTRAIT_HEIGHT), method=Image.Resampling.LANCZOS
        )
        indexes = _atkinson_indexes(portrait)
    return _pack_portrait_indexes(indexes)


def frame_to_portrait(frame: bytes) -> Image.Image:
    """Decode a packed native frame into a measured-color portrait preview."""
    if len(frame) != FRAME_BYTES:
        raise ValueError(f"frame must contain exactly {FRAME_BYTES} bytes")

    colors_by_code = {code: measured for _, measured, code in PANEL_COLORS}
    pixels = bytearray(WIDTH * HEIGHT * 3)
    output_offset = 0
    for packed in frame:
        for code in (packed >> 4, packed & 0x0F):
            try:
                color = colors_by_code[code]
            except KeyError as error:
                raise ValueError(f"unsupported panel color code: 0x{code:x}") from error
            pixels[output_offset : output_offset + 3] = bytes(color)
            output_offset += 3

    native = Image.frombytes("RGB", (WIDTH, HEIGHT), bytes(pixels))
    return native.transpose(Image.Transpose.ROTATE_90)


def frame_preview_jpeg(frame: bytes) -> bytes:
    """Encode a measured-color browser preview of the exact panel frame."""
    output = io.BytesIO()
    frame_to_portrait(frame).save(output, "JPEG", quality=92, optimize=True)
    return output.getvalue()
