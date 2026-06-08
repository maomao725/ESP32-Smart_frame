"""
Smart Photo Frame - Image Processor
Converts any uploaded image to 800x480 6-color BMP for the E6 e-paper display.
"""
from PIL import Image
import io

# E6 display native 6-color palette (RGB)
E6_PALETTE_COLORS = [
    (0,   0,   0),    # 0 - Black
    (255, 255, 255),  # 1 - White
    (255, 255, 0),    # 2 - Yellow
    (255, 0,   0),    # 3 - Red
    (0,   0,   0),    # 4 - unused (pad)
    (0,   0,   255),  # 5 - Blue
    (0,   255, 0),    # 6 - Green
]

DISPLAY_WIDTH  = 800
DISPLAY_HEIGHT = 480


def _build_palette_image() -> Image.Image:
    """Build a palette-mode image used as quantization target."""
    palette_img = Image.new("P", (1, 1))
    flat = []
    for r, g, b in E6_PALETTE_COLORS:
        flat += [r, g, b]
    # Pad to 256 colors (768 bytes)
    flat += [0] * (768 - len(flat))
    palette_img.putpalette(flat)
    return palette_img


_PALETTE_IMG = _build_palette_image()


def convert_to_e6_bmp(image_bytes: bytes) -> bytes:
    """
    Convert raw image bytes to 800x480 6-color BMP.

    Steps:
    1. Open image (any format PIL supports)
    2. Resize to 800x480 with LANCZOS resampling (preserves quality)
    3. Quantize to 6-color palette with Floyd-Steinberg dithering
    4. Return BMP bytes

    Args:
        image_bytes: Raw bytes of the source image (JPEG, PNG, etc.)

    Returns:
        BMP file bytes ready to be saved and sent to ESP32.
    """
    src = Image.open(io.BytesIO(image_bytes)).convert("RGB")

    # Resize to display resolution
    src = src.resize((DISPLAY_WIDTH, DISPLAY_HEIGHT), Image.LANCZOS)

    # Quantize to 6-color palette with dithering
    quantized = src.quantize(palette=_PALETTE_IMG, dither=Image.Dither.FLOYDSTEINBERG)

    # Export as 24-bit BMP (GUI_ReadBmp_RGB_6Color requires 24-bit)
    out = io.BytesIO()
    quantized.convert("RGB").save(out, format="BMP")
    return out.getvalue()
