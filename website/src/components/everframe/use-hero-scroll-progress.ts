'use client';

import { RefObject, useEffect, useRef } from 'react';

interface UseHeroScrollProgressOptions {
  sectionRef: RefObject<HTMLElement | null>;
  onProgress: (progress: number) => void;
}

export function useHeroScrollProgress({ sectionRef, onProgress }: UseHeroScrollProgressOptions) {
  const onProgressRef = useRef(onProgress);

  useEffect(() => {
    onProgressRef.current = onProgress;
  }, [onProgress]);

  useEffect(() => {
    let frameId: number | null = null;

    const update = () => {
      frameId = null;
      const section = sectionRef.current;
      if (!section) return;

      const scrollDistance = Math.max(section.offsetHeight - window.innerHeight, 1);
      const progress = Math.min(Math.max(-section.getBoundingClientRect().top / scrollDistance, 0), 1);
      onProgressRef.current(progress);
    };

    const requestUpdate = () => {
      if (frameId === null) frameId = window.requestAnimationFrame(update);
    };

    window.addEventListener('scroll', requestUpdate, { passive: true });
    window.addEventListener('wheel', requestUpdate, { passive: true });
    window.addEventListener('resize', requestUpdate);
    requestUpdate();

    return () => {
      window.removeEventListener('scroll', requestUpdate);
      window.removeEventListener('wheel', requestUpdate);
      window.removeEventListener('resize', requestUpdate);
      if (frameId !== null) window.cancelAnimationFrame(frameId);
    };
  }, [sectionRef]);
}
