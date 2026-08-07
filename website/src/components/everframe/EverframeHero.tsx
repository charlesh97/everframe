'use client';

import Link from 'next/link';
import { useCallback, useEffect, useRef, type PointerEvent } from 'react';
import {
  everframeDisplayCenter,
  everframeFrameGeometry,
} from '@/config/everframeFrameGeometry';
import { EverframeHeader } from './EverframeHeader';
import { FrameArtwork } from './FrameArtwork';
import styles from './everframe-hero.module.css';
import { useHeroScrollProgress } from './use-hero-scroll-progress';

const clamp = (value: number) => Math.min(Math.max(value, 0), 1);
const smoothstep = (start: number, end: number, value: number) => {
  const t = clamp((value - start) / (end - start));
  return t * t * (3 - 2 * t);
};

export function EverframeHero() {
  const sectionRef = useRef<HTMLElement>(null);
  const sceneRef = useRef<HTMLDivElement>(null);
  const frameForegroundRef = useRef<HTMLDivElement>(null);
  const headerRef = useRef<HTMLElement>(null);
  const heroCopyRef = useRef<HTMLDivElement>(null);
  const conceptStoryRef = useRef<HTMLDivElement>(null);
  const conceptStepsContainerRef = useRef<HTMLDivElement>(null);
  const conceptStepsRef = useRef<(HTMLDivElement | null)[]>([]);
  const conceptLoginRef = useRef<HTMLAnchorElement>(null);
  const progressRef = useRef(0);
  const hoverActiveRef = useRef(false);
  const reducedMotionRef = useRef(false);

  const setRevealPoint = useCallback((clientX: number, clientY: number) => {
    const scene = sceneRef.current;
    const frameForeground = frameForegroundRef.current;
    if (!scene || !frameForeground) return;

    const bounds = scene.getBoundingClientRect();
    const x = clamp((clientX - bounds.left) / bounds.width) * 100;
    const y = clamp((clientY - bounds.top) / bounds.height) * 100;
    for (const layer of [scene, frameForeground]) {
      layer.style.setProperty('--reveal-x', `${x.toFixed(2)}%`);
      layer.style.setProperty('--reveal-y', `${y.toFixed(2)}%`);
    }
  }, []);

  const applyProgress = useCallback((progress: number) => {
    progressRef.current = progress;
    const scene = sceneRef.current;
    const frameForeground = frameForegroundRef.current;
    if (!scene || !frameForeground) return;

    const reducedMotion = reducedMotionRef.current;
    const cameraProgress = reducedMotion
      ? progress >= 0.58
        ? 1
        : 0
      : smoothstep(0.24, 0.88, progress);
    const finalScale = 1.09;
    const scale = 1 + (finalScale - 1) * cameraProgress;
    const imageBlur = reducedMotion ? 0 : 0.18 * (1 - cameraProgress);
    const artworkBlur = reducedMotion ? 0 : 0.24 * (1 - cameraProgress);
    const translateX = (0.5 - everframeDisplayCenter.x) * scale * 100;
    const translateY = (0.5 - everframeDisplayCenter.y) * scale * 100;
    const scrollArtwork = reducedMotion
      ? progress >= 0.58
        ? 1
        : 0
      : smoothstep(0.45, 0.66, progress);

    for (const layer of [scene, frameForeground]) {
      layer.style.setProperty('--scene-scale', scale.toFixed(4));
      layer.style.setProperty('--scene-translate-x', `${translateX.toFixed(3)}%`);
      layer.style.setProperty('--scene-translate-y', `${translateY.toFixed(3)}%`);
      layer.style.setProperty('--image-blur', `${imageBlur.toFixed(3)}px`);
      layer.style.setProperty('--artwork-blur', `${artworkBlur.toFixed(3)}px`);
    }
    if (scrollArtwork > 0) {
      for (const layer of [scene, frameForeground]) {
        layer.style.setProperty('--reveal-x', `${(everframeDisplayCenter.x * 100).toFixed(2)}%`);
        layer.style.setProperty('--reveal-y', `${(everframeDisplayCenter.y * 100).toFixed(2)}%`);
        layer.style.setProperty('--lens-width', `${(scrollArtwork * 900).toFixed(2)}%`);
        layer.style.setProperty('--lens-height', `${(scrollArtwork * 700).toFixed(2)}%`);
      }
    } else if (!hoverActiveRef.current) {
      scene.style.setProperty('--lens-width', '0px');
      scene.style.setProperty('--lens-height', '0px');
      frameForeground.style.setProperty('--lens-width', '0px');
      frameForeground.style.setProperty('--lens-height', '0px');
    }
    headerRef.current?.style.setProperty('opacity', (1 - smoothstep(0.6, 0.8, progress)).toFixed(3));
    const heroCopy = heroCopyRef.current;
    heroCopy?.style.setProperty('opacity', (1 - smoothstep(0.38, 0.72, progress)).toFixed(3));
    heroCopy?.style.setProperty('--copy-translate-x', `${(smoothstep(0.15, 0.55, progress) * 23).toFixed(2)}vw`);

    const storyProgress = smoothstep(0.48, 0.7, progress);
    const story = conceptStoryRef.current;
    story?.style.setProperty('opacity', storyProgress.toFixed(3));
    story?.style.setProperty('--story-translate-x', `${((1 - storyProgress) * 12).toFixed(2)}vw`);
    story?.style.setProperty('--story-translate-y', `${((1 - storyProgress) * 28).toFixed(1)}px`);

    const stepsProgress = smoothstep(0.62, 0.72, progress);
    conceptStepsContainerRef.current?.style.setProperty('opacity', stepsProgress.toFixed(3));

    conceptStepsRef.current.forEach((step, index) => {
      const stepProgress = smoothstep(0.64 + index * 0.055, 0.78 + index * 0.055, progress);
      step?.style.setProperty('opacity', stepProgress.toFixed(3));
      step?.style.setProperty('--step-translate-y', `${((1 - stepProgress) * 74).toFixed(1)}px`);
    });

    const loginProgress = smoothstep(0.82, 0.92, progress);
    conceptLoginRef.current?.style.setProperty('opacity', loginProgress.toFixed(3));
    conceptLoginRef.current?.style.setProperty('--login-translate-y', `${((1 - loginProgress) * 28).toFixed(1)}px`);
  }, []);

  useHeroScrollProgress({ sectionRef, onProgress: applyProgress });

  useEffect(() => {
    const mediaQuery = window.matchMedia('(prefers-reduced-motion: reduce)');
    const updatePreference = () => {
      reducedMotionRef.current = mediaQuery.matches;
      applyProgress(progressRef.current);
    };
    updatePreference();
    mediaQuery.addEventListener('change', updatePreference);
    return () => mediaQuery.removeEventListener('change', updatePreference);
  }, [applyProgress]);

  const handleFrameEnter = useCallback((event: PointerEvent<HTMLButtonElement>) => {
    if (progressRef.current >= 0.46 || reducedMotionRef.current) return;
    hoverActiveRef.current = true;
    setRevealPoint(event.clientX, event.clientY);
    for (const layer of [sceneRef.current, frameForegroundRef.current]) {
      layer?.style.setProperty('--lens-width', '180px');
      layer?.style.setProperty('--lens-height', '134px');
    }
  }, [setRevealPoint]);

  const handleFrameMove = useCallback((event: PointerEvent<HTMLButtonElement>) => {
    if (!hoverActiveRef.current || progressRef.current >= 0.46 || reducedMotionRef.current) return;
    setRevealPoint(event.clientX, event.clientY);
  }, [setRevealPoint]);

  const handleFrameFocus = useCallback(() => {
    if (progressRef.current >= 0.46 || reducedMotionRef.current) return;
    hoverActiveRef.current = true;
    for (const layer of [sceneRef.current, frameForegroundRef.current]) {
      layer?.style.setProperty('--lens-width', '180px');
      layer?.style.setProperty('--lens-height', '134px');
    }
  }, []);

  const handleFrameLeave = useCallback(() => {
    if (progressRef.current >= 0.46 || reducedMotionRef.current) return;
    hoverActiveRef.current = false;
    for (const layer of [sceneRef.current, frameForegroundRef.current]) {
      layer?.style.setProperty('--lens-width', '0px');
      layer?.style.setProperty('--lens-height', '0px');
    }
  }, []);

  return (
    <>
      <section ref={sectionRef} className={styles.scrollSequence} aria-label="Everframe introduction">
        <div className={styles.stickyHero}>
          <EverframeHeader headerRef={headerRef} />
          <div ref={sceneRef} className={styles.scene}>
            <img
              className={styles.roomImage}
              src="/everframe/hero/room-original.png"
              alt="A calm living room with framed artwork"
            />
            <img className={styles.flowerImage} src="/everframe/hero/room-flower.png" alt="" aria-hidden="true" />
          </div>

          <div ref={heroCopyRef} className={styles.heroCopy}>
            <p className={styles.heroEyebrow}>Everframe</p>
            <h1>
              Art that<br />
              lives<br />
              with you.
            </h1>
            <p className={styles.heroDetail}>A living collection, always in view.</p>
          </div>

          <div ref={conceptStoryRef} className={styles.conceptStory}>
            <p>Everframe</p>
            <h2>Your photos. A different perspective, every day.</h2>
          </div>
          <div ref={conceptStepsContainerRef} className={styles.conceptSteps}>
            {[
              ['01', 'Bring your memories.', 'Upload the photographs you want to keep close.'],
              ['02', 'Set the feeling.', 'Choose styles, references, and moods that feel like you.'],
              ['03', 'Live with something new.', 'Wake up to an original work made for your home.'],
            ].map(([number, title, detail], index) => (
              <div
                className={styles.conceptStep}
                key={number}
                ref={(element) => {
                  conceptStepsRef.current[index] = element;
                }}
              >
                <p>{number}</p>
                <h3>{title}</h3>
                <span>{detail}</span>
              </div>
            ))}
          </div>
          <Link ref={conceptLoginRef} className={styles.conceptLogin} href="/dashboard">
            Log in
          </Link>

          <div ref={frameForegroundRef} className={styles.frameForeground}>
            <div className={styles.frameComposite} aria-hidden="true">
              <img className={styles.frameOriginal} src="/everframe/hero/frame-original.png" alt="" />
              <img className={styles.frameFlower} src="/everframe/hero/frame-flower.png" alt="" />
            </div>
            <FrameArtwork
              onPointerEnter={handleFrameEnter}
              onPointerMove={handleFrameMove}
              onPointerLeave={handleFrameLeave}
              onFocus={handleFrameFocus}
            />
          </div>
          <div className={styles.ambientVignette} aria-hidden="true" />
        </div>
      </section>
      <div className={styles.sequenceEnd} aria-hidden="true" />
    </>
  );
}
