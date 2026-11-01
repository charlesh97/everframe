export const MAX_IMPORT_BYTES = 75 * 1024 * 1024;
export const PREVIEW_MAX_EDGE = 4096;
export const FRAME_ASPECT = 480 / 800;
export const CROP_OUTPUT_WIDTH = 960;
export const CROP_OUTPUT_HEIGHT = 1600;

const ACCEPTED_EXTENSIONS = new Set(['jpg', 'jpeg', 'png', 'heic', 'heif']);
const ACCEPTED_MIME_TYPES = new Set([
  'image/jpeg',
  'image/jpg',
  'image/png',
  'image/heic',
  'image/heif',
  'application/octet-stream',
  '',
]);

export interface ImportItem {
  id: string;
  name: string;
  previewUrl: string;
  sourceType: string;
}

export interface CropPixels {
  x: number;
  y: number;
  width: number;
  height: number;
}

export interface CropSelection {
  item: ImportItem;
  cropPixels: CropPixels;
}

export function fitWithinMaxEdge(width: number, height: number, maxEdge = PREVIEW_MAX_EDGE) {
  const scale = Math.min(1, maxEdge / Math.max(width, height));
  return {
    width: Math.max(1, Math.round(width * scale)),
    height: Math.max(1, Math.round(height * scale)),
  };
}

const extensionOf = (name: string) => name.split('.').pop()?.toLowerCase() ?? '';

export function validateImportFile(file: File): string | null {
  const extension = extensionOf(file.name);

  if (!ACCEPTED_EXTENSIONS.has(extension) || !ACCEPTED_MIME_TYPES.has(file.type.toLowerCase())) {
    return `${file.name} is not a JPEG, PNG, or HEIC image.`;
  }

  if (file.size > MAX_IMPORT_BYTES) {
    return `${file.name} is larger than 75 MB.`;
  }

  return null;
}

export async function prepareImportFile(file: File): Promise<ImportItem> {
  const validationError = validateImportFile(file);
  if (validationError) throw new Error(validationError);

  const extension = extensionOf(file.name);
  const isHeicFile =
    extension === 'heic' ||
    extension === 'heif' ||
    file.type === 'image/heic' ||
    file.type === 'image/heif';

  let decodedBlob: Blob = file;
  if (isHeicFile) {
    const { heicTo } = await import('heic-to/csp');
    decodedBlob = await heicTo({
      blob: file,
      type: 'image/jpeg',
      quality: 0.92,
    });
  }

  // Preserve the entire composition while reducing decode/crop memory. The
  // crop editor works against this proportional full-image preview; only the
  // user's crop selection is applied later when producing the 960x1600 master.
  const previewBlob = await downsampleFullImage(decodedBlob);

  return {
    id: crypto.randomUUID(),
    name: file.name,
    previewUrl: URL.createObjectURL(previewBlob),
    sourceType: file.type || `image/${extension}`,
  };
}

async function downsampleFullImage(blob: Blob): Promise<Blob> {
  let bitmap: ImageBitmap | undefined;
  let fallbackUrl: string | undefined;
  let source: CanvasImageSource;
  let sourceWidth: number;
  let sourceHeight: number;

  try {
    bitmap = await createImageBitmap(blob, { imageOrientation: 'from-image' });
    source = bitmap;
    sourceWidth = bitmap.width;
    sourceHeight = bitmap.height;
  } catch {
    fallbackUrl = URL.createObjectURL(blob);
    const image = await loadImage(fallbackUrl);
    source = image;
    sourceWidth = image.naturalWidth;
    sourceHeight = image.naturalHeight;
  }

  const dimensions = fitWithinMaxEdge(sourceWidth, sourceHeight);
  const canvas = document.createElement('canvas');
  canvas.width = dimensions.width;
  canvas.height = dimensions.height;
  const context = canvas.getContext('2d');

  if (!context) {
    bitmap?.close();
    if (fallbackUrl) URL.revokeObjectURL(fallbackUrl);
    throw new Error('This browser cannot prepare the image canvas.');
  }

  context.fillStyle = '#ffffff';
  context.fillRect(0, 0, canvas.width, canvas.height);
  context.imageSmoothingEnabled = true;
  context.imageSmoothingQuality = 'high';
  context.drawImage(source, 0, 0, canvas.width, canvas.height);
  bitmap?.close();
  if (fallbackUrl) URL.revokeObjectURL(fallbackUrl);

  return canvasToBlob(canvas, 0.92);
}

function loadImage(src: string): Promise<HTMLImageElement> {
  return new Promise((resolve, reject) => {
    const image = new Image();
    image.onload = () => resolve(image);
    image.onerror = () => reject(new Error('The selected image could not be decoded.'));
    image.src = src;
  });
}

export async function cropImageToJpeg(
  src: string,
  crop: CropPixels,
  outputWidth = CROP_OUTPUT_WIDTH,
  outputHeight = CROP_OUTPUT_HEIGHT
): Promise<Blob> {
  const image = await loadImage(src);
  const canvas = document.createElement('canvas');
  canvas.width = outputWidth;
  canvas.height = outputHeight;

  const context = canvas.getContext('2d');
  if (!context) throw new Error('This browser cannot prepare the image canvas.');

  context.imageSmoothingEnabled = true;
  context.imageSmoothingQuality = 'high';
  context.drawImage(
    image,
    crop.x,
    crop.y,
    crop.width,
    crop.height,
    0,
    0,
    outputWidth,
    outputHeight
  );

  return canvasToBlob(canvas, 0.9);
}

function canvasToBlob(canvas: HTMLCanvasElement, quality: number): Promise<Blob> {
  return new Promise((resolve, reject) => {
    canvas.toBlob(
      (blob) => (blob ? resolve(blob) : reject(new Error('The cropped image could not be encoded.'))),
      'image/jpeg',
      quality
    );
  });
}

export function releaseImportItems(items: ImportItem[]) {
  items.forEach((item) => URL.revokeObjectURL(item.previewUrl));
}
