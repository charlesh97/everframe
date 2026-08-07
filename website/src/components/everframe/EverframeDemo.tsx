'use client';

import { ArrowDown } from 'lucide-react';
import { useCallback, useRef } from 'react';
import styles from './everframe-demo.module.css';
import { useHeroScrollProgress } from './use-hero-scroll-progress';

const clamp = (value: number) => Math.min(Math.max(value, 0), 1);
const smoothstep = (start: number, end: number, value: number) => {
  const t = clamp((value - start) / (end - start));
  return t * t * (3 - 2 * t);
};

export function EverframeDemo() {
  const sectionRef = useRef<HTMLElement>(null);
  const cueRef = useRef<HTMLDivElement>(null);
  const videoFrameRef = useRef<HTMLDivElement>(null);

  const applyProgress = useCallback((progress: number) => {
    const videoProgress = smoothstep(0.12, 0.54, progress);
    const cueProgress = smoothstep(0.04, 0.26, progress);
    const blur = (1 - videoProgress) * 20;

    cueRef.current?.style.setProperty('opacity', (1 - cueProgress).toFixed(3));
    cueRef.current?.style.setProperty('--cue-translate-y', `${(cueProgress * -28).toFixed(1)}px`);
    videoFrameRef.current?.style.setProperty('opacity', videoProgress.toFixed(3));
    videoFrameRef.current?.style.setProperty('--video-blur', `${blur.toFixed(2)}px`);
    videoFrameRef.current?.style.setProperty('--video-scale', (0.94 + videoProgress * 0.06).toFixed(3));
  }, []);

  useHeroScrollProgress({ sectionRef, onProgress: applyProgress });

  return (
    <section ref={sectionRef} className={styles.demoSequence} aria-labelledby="demo-heading">
      <div className={styles.stickyDemo}>
        <div ref={cueRef} className={styles.demoCue}>
          <p id="demo-heading">Watch a demo</p>
          <ArrowDown aria-hidden="true" size={18} strokeWidth={1.5} />
        </div>
        <div ref={videoFrameRef} className={styles.videoFrame}>
          <video controls autoPlay muted loop playsInline poster="/everframe/hero/room-flower.png">
            <source src="/everframe/demo-placeholder.mp4" type="video/mp4" />
            Your browser does not support the video tag.
          </video>
        </div>
      </div>
    </section>
  );
}
