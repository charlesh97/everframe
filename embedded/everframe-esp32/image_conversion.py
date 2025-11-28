#!/usr/bin/env python3
"""
Convert JPEG images to e-paper display format for EPD_7IN3F.

This script:
1. Loads a JPEG image
2. Resizes it to 480x800 (width x height)
3. Maps colors to the 7-color palette (BLACK, WHITE, GREEN, BLUE, RED, YELLOW, ORANGE)
4. Packs 2 pixels per byte (3 bits per pixel)
"""

import sys
from PIL import Image
import numpy as np

# Display resolution
EPD_WIDTH = 480
EPD_HEIGHT = 800

# Color mappings (3-bit values)
EPD_7IN3F_BLACK = 0x0   # 000
EPD_7IN3F_WHITE = 0x1   # 001
EPD_7IN3F_GREEN = 0x2   # 010
EPD_7IN3F_BLUE = 0x3    # 011
EPD_7IN3F_RED = 0x4     # 100
EPD_7IN3F_YELLOW = 0x5  # 101
EPD_7IN3F_ORANGE = 0x6  # 110

# Color palette for quantization (RGB values)
# Order: BLACK, WHITE, GREEN, BLUE, RED, YELLOW, ORANGE
PALETTE_COLORS = [
    (0, 0, 0),        # BLACK
    (255, 255, 255),  # WHITE
    (0, 255, 0),      # GREEN
    (0, 0, 255),      # BLUE
    (255, 0, 0),      # RED
    (255, 255, 0),    # YELLOW
    (255, 128, 0),    # ORANGE
]


def create_palette_image():
    """Create a PIL palette image with the 7 colors."""
    pal_image = Image.new("P", (1, 1))
    # Create palette: 256 colors, but we only use the first 7
    palette_data = []
    for r, g, b in PALETTE_COLORS:
        palette_data.extend([r, g, b])
    # Fill the rest with zeros
    palette_data.extend([0, 0, 0] * (256 - len(PALETTE_COLORS)))
    pal_image.putpalette(palette_data)
    return pal_image


def rgb_to_epd_color(r, g, b):
    """
    Map RGB color to nearest EPD color index using Euclidean distance.
    Returns the 3-bit color value.
    """
    rgb = np.array([r, g, b])
    min_dist = float('inf')
    best_color = EPD_7IN3F_BLACK
    
    for idx, (pr, pg, pb) in enumerate(PALETTE_COLORS):
        palette_rgb = np.array([pr, pg, pb])
        dist = np.linalg.norm(rgb - palette_rgb)
        if dist < min_dist:
            min_dist = dist
            best_color = idx
    
    return best_color


def convert_image(input_path, output_path=None):
    """
    Convert JPEG image to e-paper format.
    
    Args:
        input_path: Path to input JPEG image
        output_path: Path to output binary file (optional, defaults to input_path.bin)
    
    Returns:
        bytes: Packed image data (2 pixels per byte)
    """
    # Load and resize image
    img = Image.open(input_path)
    img = img.convert("RGB")
    img = img.resize((EPD_WIDTH, EPD_HEIGHT), Image.Resampling.LANCZOS)
    
    # Create palette image for quantization
    pal_image = create_palette_image()
    
    # Quantize image to 7 colors
    img_quantized = img.quantize(palette=pal_image, dither=Image.Dither.FLOYDSTEINBERG)
    
    # Convert to numpy array for processing
    img_array = np.array(img_quantized)
    
    # Map quantized indices to EPD color values (0-6)
    # The quantized image uses indices 0-6, which directly map to our color values
    epd_pixels = img_array.flatten()
    
    # Pack 2 pixels per byte (4 bits each, but only 3 bits used per pixel)
    # Format matches C code: (pixel0 << 4) | pixel1
    # Upper nibble (bits 7-4): pixel0, Lower nibble (bits 3-0): pixel1
    packed_data = bytearray()
    
    for i in range(0, len(epd_pixels), 2):
        pixel0 = epd_pixels[i] & 0x07  # Ensure 3 bits
        if i + 1 < len(epd_pixels):
            pixel1 = epd_pixels[i + 1] & 0x07  # Ensure 3 bits
        else:
            pixel1 = EPD_7IN3F_WHITE  # Padding with white if odd number of pixels
        
        # Pack: pixel0 in upper nibble (bits 7-4), pixel1 in lower nibble (bits 3-0)
        # This matches the C code format: (color<<4)|color
        byte_value = (pixel0 << 4) | pixel1
        packed_data.append(byte_value)
    
    # Write to file if output path specified
    if output_path: 
        with open(output_path, 'wb') as f:
            f.write(packed_data)
        print(f"Converted image saved to: {output_path}")
        print(f"Image size: {EPD_WIDTH}x{EPD_HEIGHT}")
        print(f"Packed data size: {len(packed_data)} bytes")
        print(f"Expected size: {EPD_WIDTH * EPD_HEIGHT // 2} bytes")
    
    return bytes(packed_data)


def main():
    """Main function for command-line usage."""
    if len(sys.argv) < 2:
        print("Usage: python image_conversion.py <input.jpg> [output.bin]")
        print("\nConverts a JPEG image to e-paper display format:")
        print("  - Resizes to 480x800 (width x height)")
        print("  - Maps to 7-color palette")
        print("  - Packs 2 pixels per byte (3 bits each)")
        sys.exit(1)
    
    input_path = sys.argv[1]
    output_path = sys.argv[2] if len(sys.argv) > 2 else input_path.rsplit('.', 1)[0] + '.bin'
    
    try:
        convert_image(input_path, output_path)
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

