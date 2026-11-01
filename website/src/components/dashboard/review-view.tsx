'use client';

import { ArrowLeft, ArrowRight, Upload } from 'lucide-react';
import { useMemo, useState } from 'react';
import Cropper, { type Area } from 'react-easy-crop';
import { Button } from '@/components/ui/button';
import { FRAME_ASPECT, type CropSelection, type ImportItem } from '@/lib/image-import';
import styles from './dashboard.module.css';

interface CropEdit {
  crop: { x: number; y: number };
  zoom: number;
  pixels?: Area;
}

interface ReviewViewProps {
  items: ImportItem[];
  onComplete: (selected: CropSelection[]) => void;
  onCancel: () => void;
}

export default function ReviewView({ items, onComplete, onCancel }: ReviewViewProps) {
  const [currentIndex, setCurrentIndex] = useState(0);
  const [edits, setEdits] = useState<Record<string, CropEdit>>({});
  const current = items[currentIndex];
  const currentEdit = edits[current?.id] ?? { crop: { x: 0, y: 0 }, zoom: 1 };

  const allCropsReady = useMemo(
    () => items.every((item) => Boolean(edits[item.id]?.pixels)),
    [edits, items]
  );

  if (!current) return null;

  const updateCurrent = (update: Partial<CropEdit>) => {
    setEdits((previous) => {
      const existing = previous[current.id];
      return {
        ...previous,
        [current.id]: {
          crop: existing?.crop ?? { x: 0, y: 0 },
          zoom: existing?.zoom ?? 1,
          pixels: existing?.pixels,
          ...update,
        },
      };
    });
  };

  const upload = () => {
    if (!allCropsReady) return;
    onComplete(
      items.map((item) => ({
        item,
        cropPixels: edits[item.id].pixels!,
      }))
    );
  };

  return (
    <main className={`${styles.main} animate-in fade-in`}>
      <div className={styles.cropHeader}>
        <div>
          <p className={styles.eyebrow}>Photo {currentIndex + 1} of {items.length}</p>
          <h1 className={styles.sectionTitle}>Choose the crop</h1>
          <p className={styles.bodyText}>
            Drag to reposition. Pinch or use the slider to zoom.
          </p>
        </div>
        <p className={styles.cropFileName}>{current.name}</p>
      </div>

      <div className={styles.cropPanel}>
        <div className={styles.cropCanvas}>
          <Cropper
            image={current.previewUrl}
            crop={currentEdit.crop}
            zoom={currentEdit.zoom}
            aspect={FRAME_ASPECT}
            objectFit="contain"
            showGrid
            onCropChange={(crop) => updateCurrent({ crop })}
            onZoomChange={(zoom) => updateCurrent({ zoom })}
            onCropComplete={(_, pixels) => updateCurrent({ pixels })}
          />
        </div>

        <div className={styles.cropControls}>
          <label className={styles.zoomControl}>
            <span>Zoom</span>
            <input
              aria-label="Crop zoom"
              type="range"
              min={1}
              max={2.5}
              step={0.01}
              value={currentEdit.zoom}
              onChange={(event) => updateCurrent({ zoom: Number(event.target.value) })}
            />
          </label>

          <div className={styles.cropActions}>
            <Button type="button" variant="ghost" onClick={onCancel}>Cancel</Button>
            {currentIndex > 0 && (
              <Button
                type="button"
                variant="secondary"
                onClick={() => setCurrentIndex((index) => index - 1)}
              >
                <ArrowLeft strokeWidth={1.5} /> Back
              </Button>
            )}
            {currentIndex < items.length - 1 ? (
              <Button
                type="button"
                className={styles.primaryButton}
                disabled={!currentEdit.pixels}
                onClick={() => setCurrentIndex((index) => index + 1)}
              >
                Next <ArrowRight strokeWidth={1.5} />
              </Button>
            ) : (
              <Button
                type="button"
                className={styles.primaryButton}
                disabled={!allCropsReady}
                onClick={upload}
              >
                <Upload strokeWidth={1.5} /> Upload {items.length === 1 ? 'photo' : `${items.length} photos`}
              </Button>
            )}
          </div>
        </div>
      </div>

      <p className={styles.cropNote}>
        This portrait crop matches Everframe. The cloud rotates it into the panel's native 800 × 480 buffer.
      </p>
    </main>
  );
}
