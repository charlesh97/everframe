#!/usr/bin/env node

import {mkdir, writeFile} from 'node:fs/promises';
import path from 'node:path';
import process from 'node:process';
import dotenv from 'dotenv';

dotenv.config({path: '.env.local'});
dotenv.config();

const MODEL = process.env.NANO_BANANA_MODEL || 'nano-banana-lite';
const API_KEY = process.env.NANO_BANANA_API_KEY || process.env.GOOGLE_GENAI_API_KEY;
const OUT_DIR = path.resolve('public/everframe/hero/generated');
const DRY_RUN = process.argv.includes('--dry-run');
const LIMIT = Number.parseInt(getArgValue('--limit') || '20', 10);

const subjects = [
  {
    slug: 'architect-with-scale-model',
    subject: 'a thoughtful architect holding a small wood-and-paper scale model in a bright studio',
    category: 'person',
  },
  {
    slug: 'family-breakfast-window',
    subject: 'a candid family breakfast at a sunlit kitchen window, coffee cups, toast, and soft morning light',
    category: 'people',
  },
  {
    slug: 'ceramic-vase-wildflowers',
    subject: 'a handmade ceramic vase filled with loose wildflowers on a clean linen-covered table',
    category: 'item',
  },
  {
    slug: 'moonlit-coastal-town',
    subject: 'a small coastal town at dusk with warm windows, wet stone streets, and a moonlit harbor',
    category: 'place',
  },
  {
    slug: 'vintage-camera-desk',
    subject: 'a vintage film camera, contact sheets, pencils, and handwritten notes on a walnut desk',
    category: 'item',
  },
  {
    slug: 'museum-courtyard',
    subject: 'a quiet museum courtyard with olive trees, stone benches, and visitors moving through late afternoon light',
    category: 'place',
  },
  {
    slug: 'musician-green-room',
    subject: 'a musician tuning an old hollow-body guitar backstage beside a mirror framed with warm bulbs',
    category: 'person',
  },
  {
    slug: 'mountain-observatory',
    subject: 'a remote mountain observatory above clouds under a clear indigo sky',
    category: 'place',
  },
  {
    slug: 'fruit-bowl-market',
    subject: 'a bowl of citrus, figs, grapes, and pears on a market stall with patterned cloth',
    category: 'item',
  },
  {
    slug: 'gardener-glasshouse',
    subject: 'a gardener misting rare plants inside an old glasshouse filled with condensation and green light',
    category: 'person',
  },
  {
    slug: 'desert-gas-station',
    subject: 'a lonely desert gas station at blue hour with a parked classic car and neon reflections',
    category: 'place',
  },
  {
    slug: 'chair-reading-corner',
    subject: 'a sculptural reading chair beside stacked books, a floor lamp, and a textured wool rug',
    category: 'item',
  },
  {
    slug: 'chef-plating-dessert',
    subject: 'a chef carefully plating a berry dessert with tweezers in a calm restaurant kitchen',
    category: 'person',
  },
  {
    slug: 'rainy-city-crosswalk',
    subject: 'a rainy city crosswalk with umbrellas, taxi headlights, storefront reflections, and motion blur',
    category: 'place',
  },
  {
    slug: 'bicycle-leaning-wall',
    subject: 'a matte black bicycle leaning against a textured plaster wall with afternoon shadows',
    category: 'item',
  },
  {
    slug: 'dancer-rehearsal-room',
    subject: 'a dancer resting between rehearsals in a mirrored room with tape marks on the floor',
    category: 'person',
  },
  {
    slug: 'alpine-cabin-snow',
    subject: 'a compact alpine cabin after fresh snow, smoke from the chimney, pine trees, and amber interior light',
    category: 'place',
  },
  {
    slug: 'desk-lamp-blueprint',
    subject: 'an adjustable brass desk lamp illuminating blueprints, calipers, and a small prototype part',
    category: 'item',
  },
  {
    slug: 'portrait-red-scarf',
    subject: 'a calm portrait of an older woman in a red scarf standing near a tiled station entrance',
    category: 'person',
  },
  {
    slug: 'botanical-library',
    subject: 'a botanical library with tall shelves, specimen drawers, ladders, and pressed plants on a worktable',
    category: 'place',
  },
];

const styles = [
  'watercolor painting',
  'black and white charcoal sketch',
  'vaporwave digital art',
  'oil painting with thick impasto',
  'minimalist vector art',
  'vintage polaroid',
  'ukiyo-e woodblock print',
  'colored pencil illustration',
  'soft pastel portrait',
  'digital concept art',
  'matte painting style',
  'cinematic lighting photograph',
  'architectural blueprint drawing',
  'hand-inked line art',
  'marker sketch rendering',
  'studio photography aesthetic',
  'sepia-toned vintage print',
  'modernist geometric abstraction',
  'retro magazine illustration',
  'film noir still frame',
  'photorealistic oil painting',
  'fine art graphite drawing',
  'digital cartoon shading',
  'airbrush poster style',
  'screenprint poster art',
  'low-poly 3D model render',
  'handmade clay sculpture',
  'hand-tinted black and white photo',
  'Renaissance portrait painting',
  'soft-focus analog photograph',
  'gouache painting on paper',
  'etching print on aged paper',
  'colorized 1950s photograph',
  'hyperrealistic digital render',
  'pastel chalkboard drawing',
  'pen and ink architectural sketch',
  'acrylic impasto landscape',
  'studio lighting product shot',
  'pencil contour drawing',
  'isometric digital illustration',
  'plaster bust sculpture',
  'clay model still life',
  'studio portrait with Rembrandt lighting',
  'flat design vector illustration',
  'hand-painted sign lettering',
  'posterized photo illustration',
  'manga-style comic panel',
  'animation cel artwork',
  'engraved copperplate illustration',
  'traditional Chinese brush painting',
  'modern watercolor wash',
  'black ink wash study',
  'vintage postcard print',
  'collage mixed-media layout',
  'tinted cyanotype photograph',
  'mid-century advertising illustration',
  'minimal product render',
  'CGI photoreal render',
  'digital blueprint wireframe',
  'concept sketch with rough pencil',
  'handmade ceramic glaze finish',
  'woodcut print reproduction',
  'industrial design rendering',
  'early 1900s portrait photograph',
  'impressionist brushstroke style',
  'chalk pastel life drawing',
  'digital portrait illustration',
  'storyboard frame drawing',
  'technical cross-section illustration',
  'paper cut-out collage',
  'children’s book watercolor',
  'realistic 3D render with studio lighting',
  'macro photograph with shallow depth of field',
  'architectural elevation drawing',
  'monochrome fine art print',
  'film grain black and white photograph',
  'commercial product visualization',
  'charcoal tonal sketch',
  'sumi-e ink brush painting',
  'ceramic bust sculpture',
  'hand-drawn topographic map',
  'vintage travel poster design',
  'digital flat shading illustration',
  'mid-century graphic poster',
  'real-time game asset render',
  'studio macro texture photo',
  'architectural concept rendering',
  'photoreal still life photograph',
  'wooden model maquette',
  'digital pastel drawing',
  'studio-lit food photograph',
  'precise ink technical illustration',
  'vintage lithograph print',
  '3D wireframe visualization',
  'fashion editorial photo lighting',
  'hand-painted watercolor pattern',
  'inked comic-style contour drawing',
  'colored marker sketch concept',
  'architectural pencil rendering',
  'traditional oil still life painting',
];

function getArgValue(name) {
  const arg = process.argv.find(value => value.startsWith(`${name}=`));
  return arg?.slice(name.length + 1);
}

function shuffle(values) {
  const shuffled = [...values];
  for (let i = shuffled.length - 1; i > 0; i -= 1) {
    const j = Math.floor(Math.random() * (i + 1));
    [shuffled[i], shuffled[j]] = [shuffled[j], shuffled[i]];
  }
  return shuffled;
}

function selectRandomSamples() {
  const shuffledSubjects = shuffle(subjects);
  const shuffledStyles = shuffle(styles);

  return Array.from({length: LIMIT}, (_, index) => ({
    ...shuffledSubjects[index % shuffledSubjects.length],
    style: shuffledStyles[index % shuffledStyles.length],
  }));
}

function buildPrompt({subject, style}) {
  return [
    `Create an original portrait-oriented hero artwork insert for the Everframe landing page: ${subject}.`,
    `Render it in the style of ${style}.`,
    'Make it premium, warm, artful, and suitable for display inside a modern digital picture frame.',
    'No text, logos, watermarks, UI, captions, borders, or frames.',
    'Use a 4:5 portrait composition. Final target: 1200x1500 pixels.',
  ].join(' ');
}

function getImagePart(response) {
  const parts = response?.candidates?.flatMap(candidate => candidate?.content?.parts || []) || [];
  return parts.find(part => part?.inlineData?.data || part?.inline_data?.data);
}

function getMimeType(part) {
  return part?.inlineData?.mimeType || part?.inline_data?.mime_type || 'image/png';
}

function getExtension(mimeType) {
  if (mimeType.includes('jpeg') || mimeType.includes('jpg')) return 'jpg';
  if (mimeType.includes('webp')) return 'webp';
  return 'png';
}

async function generateImage(sample, index) {
  const prompt = buildPrompt(sample);
  const url = `https://generativelanguage.googleapis.com/v1beta/models/${encodeURIComponent(MODEL)}:generateContent?key=${encodeURIComponent(API_KEY)}`;
  const body = {
    contents: [
      {
        role: 'user',
        parts: [{text: prompt}],
      },
    ],
    generationConfig: {
      responseModalities: ['TEXT', 'IMAGE'],
      imageConfig: {
        aspectRatio: '4:5',
      },
    },
  };

  const response = await fetch(url, {
    method: 'POST',
    headers: {'Content-Type': 'application/json'},
    body: JSON.stringify(body),
  });

  const json = await response.json();
  if (!response.ok) {
    throw new Error(`Generation failed for ${sample.slug}: ${response.status} ${JSON.stringify(json)}`);
  }

  const part = getImagePart(json);
  if (!part) {
    throw new Error(`No image returned for ${sample.slug}: ${JSON.stringify(json)}`);
  }

  const mimeType = getMimeType(part);
  const data = part.inlineData?.data || part.inline_data?.data;
  const extension = getExtension(mimeType);
  const filename = `${String(index + 1).padStart(2, '0')}-${sample.slug}.${extension}`;
  const filepath = path.join(OUT_DIR, filename);

  await writeFile(filepath, Buffer.from(data, 'base64'));
  return {
    ...sample,
    file: `/everframe/hero/generated/${filename}`,
    model: MODEL,
    mimeType,
    prompt,
  };
}

async function main() {
  if (!Number.isFinite(LIMIT) || LIMIT < 1) {
    throw new Error('--limit must be a positive number');
  }

  const selected = selectRandomSamples();

  if (DRY_RUN) {
    console.log(`Randomly paired ${selected.length} subjects with ${styles.length} hardcoded style prompts.`);
    selected.forEach((sample, index) => {
      console.log(`\n${String(index + 1).padStart(2, '0')}. ${sample.slug} | style: ${sample.style}`);
      console.log(buildPrompt(sample));
    });
    return;
  }

  if (!API_KEY) {
    throw new Error('Set NANO_BANANA_API_KEY or GOOGLE_GENAI_API_KEY in website/.env.local before running this script.');
  }

  await mkdir(OUT_DIR, {recursive: true});

  const results = [];
  for (const [index, sample] of selected.entries()) {
    console.log(`Generating ${index + 1}/${selected.length}: ${sample.slug}`);
    results.push(await generateImage(sample, index));
  }

  const metadataPath = path.join(OUT_DIR, 'metadata.json');
  await writeFile(
    metadataPath,
    `${JSON.stringify(
      {
        generatedAt: new Date().toISOString(),
        model: MODEL,
        subjectCount: subjects.length,
        styleCount: styles.length,
        images: results,
      },
      null,
      2
    )}\n`
  );

  console.log(`Saved ${results.length} images and metadata to ${OUT_DIR}`);
}

main().catch(error => {
  console.error(error);
  process.exit(1);
});
