'use server';
/**
 * @fileOverview Transforms an image into a single abstract art variation using Gemini Pro.
 *
 * - transformImageToAbstractArt - A function that handles the image transformation process.
 * - TransformImageToAbstractArtInput - The input type for the transformImageToAbstractArt function.
 * - TransformImageToAbstractArtOutput - The return type for the transformImageToabstractArt function.
 */

import {ai} from '@/ai/genkit';
import {z} from 'genkit';

const PROMPTS = [
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

const TransformImageToAbstractArtInputSchema = z.object({
  photoDataUri: z
    .string()
    .describe(
      'A photo to be transformed into abstract art, as a data URI that must include a MIME type and use Base64 encoding. Expected format: data:<mimetype>;base64,<encoded_data>.'
    ),
});
export type TransformImageToAbstractArtInput = z.infer<
  typeof TransformImageToAbstractArtInputSchema
>;

const TransformImageToAbstractArtOutputSchema = z.object({
  abstractArtUrl: z
    .string()
    .describe(
      'A data URI representing the abstract art variation of the original photo.'
    ),
  style: z.string().describe('The artistic style used for the transformation.'),
});
export type TransformImageToAbstractArtOutput = z.infer<
  typeof TransformImageToAbstractArtOutputSchema
>;

export async function transformImageToAbstractArt(
  input: TransformImageToAbstractArtInput
): Promise<TransformImageToAbstractArtOutput> {
  return transformImageToAbstractArtFlow(input);
}

const transformImageToAbstractArtFlow = ai.defineFlow(
  {
    name: 'transformImageToAbstractArtFlow',
    inputSchema: TransformImageToAbstractArtInputSchema,
    outputSchema: TransformImageToAbstractArtOutputSchema,
  },
  async input => {
    const idx = Math.floor(Math.random() * PROMPTS.length)
    const style = PROMPTS[idx];
    console.info('Using style:', style, 'Using index:', idx)

    const {media} = await ai.generate({
      model: 'googleai/gemini-3-pro-image-preview',
      prompt: [
        {media: {url: input.photoDataUri}},
        {
          text: `Recreate this photo in the style of ${style}. High quality, artistic, highly detailed. Crop to 480x800 pixels.`,
        },
      ],
      config: {
        responseModalities: ['TEXT', 'IMAGE'],
      },
    });

    return {
      abstractArtUrl:
        media?.url ||
        'data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNkYAAAAAYAAjCB0C8AAAAASUVORK5CYII=',
      style: style,
    };
  }
);
