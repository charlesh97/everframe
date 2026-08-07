import type { PointerEvent } from 'react';
import { everframeFrameGeometry } from '@/config/everframeFrameGeometry';
import styles from './everframe-hero.module.css';

interface FrameArtworkProps {
  onPointerEnter: (event: PointerEvent<HTMLButtonElement>) => void;
  onPointerMove: (event: PointerEvent<HTMLButtonElement>) => void;
  onPointerLeave: () => void;
  onFocus: () => void;
}

export function FrameArtwork({ onPointerEnter, onPointerMove, onPointerLeave, onFocus }: FrameArtworkProps) {
  const { display, clipPath } = everframeFrameGeometry;

  return (
    <button
      className={styles.frameHotspot}
      style={{
        left: `${display.left * 100}%`,
        top: `${display.top * 100}%`,
        width: `${display.width * 100}%`,
        height: `${display.height * 100}%`,
        clipPath,
      }}
      type="button"
      aria-label="Preview Everframe changing artwork"
      onPointerEnter={onPointerEnter}
      onPointerMove={onPointerMove}
      onPointerLeave={onPointerLeave}
      onFocus={onFocus}
      onBlur={onPointerLeave}
    />
  );
}
