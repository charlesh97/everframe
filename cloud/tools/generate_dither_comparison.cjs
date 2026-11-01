#!/usr/bin/env node
/*
 * Generate a controlled four-way Spectra 6 dithering comparison.
 *
 * Dependencies:
 *   npm install sharp @opendisplay/epaper-dithering
 *
 * The measured palette, OKLab matching, Burkes, and Atkinson implementations
 * come from OpenDisplay's MIT-licensed epaper-dithering package. Candidate 2
 * uses Christoph Peters' CC0 void-and-cluster blue-noise threshold mask and
 * the two nearest measured-palette colors in weighted OKLab space.
 */

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const sharp = require("sharp");
const {
  DitherMode,
  SPECTRA_7_3_6COLOR_V2,
  ditherImage,
} = require("@opendisplay/epaper-dithering");

const WIDTH = 480;
const HEIGHT = 800;
const NATIVE_WIDTH = 800;
const NATIVE_HEIGHT = 480;
const WAB = 1.5;
const INK_CODES = [0x0, 0x1, 0x2, 0x3, 0x5, 0x6];
const PALETTE = Object.values(SPECTRA_7_3_6COLOR_V2.colors);

function srgbToLinear(value) {
  const c = value / 255;
  return c <= 0.04045 ? c / 12.92 : ((c + 0.055) / 1.055) ** 2.4;
}

function linearToSrgb(value) {
  const c = Math.max(0, Math.min(1, value));
  const encoded = c <= 0.0031308 ? 12.92 * c : 1.055 * c ** (1 / 2.4) - 0.055;
  return Math.round(encoded * 255);
}

function linearRgbToOklab(r, g, b) {
  const l = 0.4122214708 * r + 0.5363325363 * g + 0.0514459929 * b;
  const m = 0.2119034982 * r + 0.6806995451 * g + 0.1073969566 * b;
  const s = 0.0883024619 * r + 0.2817188376 * g + 0.6299787005 * b;
  const lc = Math.cbrt(l);
  const mc = Math.cbrt(m);
  const sc = Math.cbrt(s);
  return [
    0.2104542553 * lc + 0.7936177850 * mc - 0.0040720468 * sc,
    1.9779984951 * lc - 2.4285922050 * mc + 0.4505937099 * sc,
    0.0259040371 * lc + 0.7827717662 * mc - 0.8086757660 * sc,
  ];
}

function rgbToOklab(r, g, b) {
  return linearRgbToOklab(srgbToLinear(r), srgbToLinear(g), srgbToLinear(b));
}

const PALETTE_LAB = PALETTE.map(({ r, g, b }) => rgbToOklab(r, g, b));

function distanceSquared(lab, paletteLab) {
  const dl = lab[0] - paletteLab[0];
  const da = WAB * (lab[1] - paletteLab[1]);
  const db = WAB * (lab[2] - paletteLab[2]);
  return dl * dl + da * da + db * db;
}

function toneCompress(rgba) {
  const result = new Uint8ClampedArray(rgba.length);
  const black = PALETTE[0];
  const white = PALETTE[1];
  const luminance = (r, g, b) => 0.2126729 * r + 0.7151522 * g + 0.0721750 * b;
  const blackY = luminance(
    srgbToLinear(black.r), srgbToLinear(black.g), srgbToLinear(black.b)
  );
  const whiteY = luminance(
    srgbToLinear(white.r), srgbToLinear(white.g), srgbToLinear(white.b)
  );

  for (let offset = 0; offset < rgba.length; offset += 4) {
    const linear = [
      srgbToLinear(rgba[offset]),
      srgbToLinear(rgba[offset + 1]),
      srgbToLinear(rgba[offset + 2]),
    ];
    const y = luminance(...linear);
    const targetY = blackY + y * (whiteY - blackY);
    const scale = y > 1e-8 ? targetY / y : 0;
    result[offset] = linearToSrgb(y > 1e-8 ? linear[0] * scale : blackY);
    result[offset + 1] = linearToSrgb(y > 1e-8 ? linear[1] * scale : blackY);
    result[offset + 2] = linearToSrgb(y > 1e-8 ? linear[2] * scale : blackY);
    result[offset + 3] = 255;
  }
  return result;
}

function blueNoiseDither(rgba, tile) {
  const indices = new Uint8Array(WIDTH * HEIGHT);
  const { size, thresholds } = tile;
  for (let y = 0; y < HEIGHT; y += 1) {
    for (let x = 0; x < WIDTH; x += 1) {
      const pixel = y * WIDTH + x;
      const offset = pixel * 4;
      const lab = rgbToOklab(rgba[offset], rgba[offset + 1], rgba[offset + 2]);
      const ranked = PALETTE_LAB.map((color, index) => ({
        index,
        distance: distanceSquared(lab, color),
      })).sort((a, b) => a.distance - b.distance);
      const nearest = ranked[0];
      const second = ranked[1];
      const denominator = nearest.distance + second.distance;
      const nearestProbability = denominator > 1e-12
        ? second.distance / denominator
        : 1;
      const threshold = thresholds[(y % size) * size + (x % size)];
      indices[pixel] = threshold < nearestProbability ? nearest.index : second.index;
    }
  }
  return indices;
}

const DIGITS = {
  1: ["010", "110", "010", "010", "010", "010", "111"],
  2: ["111", "001", "001", "111", "100", "100", "111"],
  3: ["111", "001", "001", "111", "001", "001", "111"],
  4: ["101", "101", "101", "111", "001", "001", "001"],
};

function addNumber(indices, number) {
  const x0 = 12;
  const y0 = 12;
  const boxWidth = 62;
  const boxHeight = 98;
  const scale = 10;
  for (let y = y0; y < y0 + boxHeight; y += 1) {
    for (let x = x0; x < x0 + boxWidth; x += 1) {
      const border = x < x0 + 3 || x >= x0 + boxWidth - 3 ||
        y < y0 + 3 || y >= y0 + boxHeight - 3;
      indices[y * WIDTH + x] = border ? 0 : 1;
    }
  }
  DIGITS[number].forEach((row, gy) => {
    [...row].forEach((bit, gx) => {
      if (bit !== "1") return;
      for (let py = 0; py < scale; py += 1) {
        for (let px = 0; px < scale; px += 1) {
          const x = x0 + 16 + gx * scale + px;
          const y = y0 + 14 + gy * scale + py;
          indices[y * WIDTH + x] = 0;
        }
      }
    });
  });
}

function indicesToPreview(indices) {
  const rgb = Buffer.alloc(WIDTH * HEIGHT * 3);
  for (let pixel = 0; pixel < indices.length; pixel += 1) {
    const color = PALETTE[indices[pixel]];
    rgb[pixel * 3] = color.r;
    rgb[pixel * 3 + 1] = color.g;
    rgb[pixel * 3 + 2] = color.b;
  }
  return rgb;
}

function indicesToFrame(indices) {
  const native = new Uint8Array(NATIVE_WIDTH * NATIVE_HEIGHT);
  for (let y = 0; y < HEIGHT; y += 1) {
    for (let x = 0; x < WIDTH; x += 1) {
      const nativeX = NATIVE_WIDTH - 1 - y;
      const nativeY = x;
      native[nativeY * NATIVE_WIDTH + nativeX] = indices[y * WIDTH + x];
    }
  }
  const frame = Buffer.alloc(NATIVE_WIDTH * NATIVE_HEIGHT / 2);
  for (let pixel = 0; pixel < native.length; pixel += 2) {
    frame[pixel / 2] = (INK_CODES[native[pixel]] << 4) | INK_CODES[native[pixel + 1]];
  }
  return frame;
}

function xmlEscape(value) {
  return value.replaceAll("&", "&amp;").replaceAll("<", "&lt;").replaceAll(">", "&gt;");
}

async function main() {
  const input = process.argv[2];
  const outputDirectory = process.argv[3];
  const addLabels = !process.argv.includes("--no-labels");
  if (!input || !outputDirectory) {
    throw new Error("usage: generate_dither_comparison.cjs INPUT_IMAGE OUTPUT_DIRECTORY");
  }
  fs.mkdirSync(outputDirectory, { recursive: true });

  const { data, info } = await sharp(input)
    .rotate()
    .resize(WIDTH, HEIGHT, { fit: "cover", position: "centre" })
    .ensureAlpha()
    .raw()
    .toBuffer({ resolveWithObject: true });
  if (info.width !== WIDTH || info.height !== HEIGHT || info.channels !== 4) {
    throw new Error(`unexpected source raster: ${JSON.stringify(info)}`);
  }
  const prepared = toneCompress(new Uint8ClampedArray(data));
  const imageBuffer = { width: WIDTH, height: HEIGHT, data: prepared };
  const maskPath = path.join(__dirname, "assets", "blue-noise-64.png");
  const mask = await sharp(maskPath).greyscale().raw().toBuffer({ resolveWithObject: true });
  if (mask.info.width !== mask.info.height || mask.info.channels !== 1) {
    throw new Error(`unexpected blue-noise mask: ${JSON.stringify(mask.info)}`);
  }
  const blueNoise = {
    size: mask.info.width,
    thresholds: Float64Array.from(mask.data, (value) => (value + 0.5) / 256),
  };
  const candidates = [
    {
      number: 1,
      slug: "no_dither",
      label: "No dither",
      indices: ditherImage(imageBuffer, SPECTRA_7_3_6COLOR_V2, {
        mode: DitherMode.NONE,
      }).indices,
    },
    {
      number: 2,
      slug: "blue_noise",
      label: "Blue-noise",
      indices: blueNoiseDither(prepared, blueNoise),
    },
    {
      number: 3,
      slug: "burkes",
      label: "Serpentine Burkes",
      indices: ditherImage(imageBuffer, SPECTRA_7_3_6COLOR_V2, {
        mode: DitherMode.BURKES,
        serpentine: true,
      }).indices,
    },
    {
      number: 4,
      slug: "atkinson",
      label: "Serpentine Atkinson",
      indices: ditherImage(imageBuffer, SPECTRA_7_3_6COLOR_V2, {
        mode: DitherMode.ATKINSON,
        serpentine: true,
      }).indices,
    },
  ];

  const manifest = [];
  for (const candidate of candidates) {
    if (addLabels) addNumber(candidate.indices, candidate.number);
    const base = `option_${candidate.number}_${candidate.slug}`;
    const frame = indicesToFrame(candidate.indices);
    const digest = crypto.createHash("sha256").update(frame).digest("hex");
    fs.writeFileSync(path.join(outputDirectory, `${base}.bin`), frame);
    await sharp(indicesToPreview(candidate.indices), {
      raw: { width: WIDTH, height: HEIGHT, channels: 3 },
    }).png().toFile(path.join(outputDirectory, `${base}.png`));
    manifest.push({
      number: candidate.number,
      name: candidate.label,
      file: `${base}.bin`,
      sha256: digest,
      bytes: frame.length,
    });
  }

  const headerHeight = 74;
  const sheet = sharp({
    create: {
      width: WIDTH * candidates.length,
      height: HEIGHT + headerHeight,
      channels: 3,
      background: { r: 242, g: 240, b: 234 },
    },
  });
  const composites = [];
  for (const candidate of candidates) {
    const left = (candidate.number - 1) * WIDTH;
    composites.push({
      input: path.join(outputDirectory, `option_${candidate.number}_${candidate.slug}.png`),
      left,
      top: headerHeight,
    });
    const title = `${candidate.number}. ${candidate.label}`;
    composites.push({
      input: Buffer.from(`<svg width="${WIDTH}" height="${headerHeight}">
        <rect width="100%" height="100%" fill="#f2f0ea"/>
        <text x="24" y="47" font-family="Arial, sans-serif" font-size="28" font-weight="700" fill="#181818">${xmlEscape(title)}</text>
      </svg>`),
      left,
      top: 0,
    });
  }
  await sheet.composite(composites).png().toFile(path.join(outputDirectory, "comparison.png"));
  fs.writeFileSync(
    path.join(outputDirectory, "manifest.json"),
    `${JSON.stringify({ palette: PALETTE, candidates: manifest }, null, 2)}\n`,
  );
  console.log(JSON.stringify(manifest, null, 2));
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
