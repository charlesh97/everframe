/**
 * Display coordinates are normalized against room.png (5712 x 4284).
 * Keep all visual tuning here so scene composition and camera math stay aligned.
 */
export const everframeFrameGeometry = {
  sourceAspectRatio: 4 / 3,
  display: {
    left: 0.3808,
    top: 0.1945,
    width: 0.2692,
    height: 0.496,
  },
  outer: {
    left: 0.352,
    top: 0.191,
    width: 0.307,
    height: 0.526,
  },
  clipPath: 'polygon(0 0, 100% 2.2%, 100% 97.3%, 0 100%)',
} as const;

export const everframeDisplayCenter = {
  x: everframeFrameGeometry.display.left + everframeFrameGeometry.display.width / 2,
  y: everframeFrameGeometry.display.top + everframeFrameGeometry.display.height / 2,
} as const;

const outer = everframeFrameGeometry.outer;

export const everframeOuterFrameClipPath = `polygon(
  ${outer.left * 100}% ${outer.top * 100}%,
  ${(outer.left + outer.width) * 100}% ${(outer.top + 0.024) * 100}%,
  ${(outer.left + outer.width) * 100}% ${(outer.top + outer.height) * 100}%,
  ${outer.left * 100}% ${(outer.top + outer.height) * 100}%
)`;
