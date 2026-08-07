'use client';

import { ArrowRight, CheckCircle } from 'lucide-react';
import Image from 'next/image';
import { useState } from 'react';
import type { ReviewItem } from '@/app/dashboard/page';
import { Button } from '@/components/ui/button';
import { Card, CardContent } from '@/components/ui/card';
import { Checkbox } from '@/components/ui/checkbox';
import styles from './dashboard.module.css';

interface ReviewViewProps {
  items: ReviewItem[];
  onComplete: (
    selected: { original: ReviewItem['original']; variationUrl: string }[]
  ) => void;
}

export default function ReviewView({ items, onComplete }: ReviewViewProps) {
  const [selectedVariations, setSelectedVariations] = useState<{
    [originalUrl: string]: string | undefined;
  }>(() => {
    const initialState: { [originalUrl: string]: string } = {};
    items.forEach((item) => {
      initialState[item.original.url] = item.variation.url; // Pre-select all
    });
    return initialState;
  });

  const handleSelectionChange = (
    originalUrl: string,
    variationUrl: string,
    isChecked: boolean
  ) => {
    setSelectedVariations((prev) => ({
      ...prev,
      [originalUrl]: isChecked ? variationUrl : undefined,
    }));
  };

  const handleFinalize = () => {
    const flattenedSelection: {
      original: ReviewItem['original'];
      variationUrl: string;
    }[] = [];
    items.forEach((item) => {
      const originalUrl = item.original.url;
      const selectedVariation = selectedVariations[originalUrl];
      if (selectedVariation) {
        flattenedSelection.push({
          original: item.original,
          variationUrl: selectedVariation,
        });
      }
    });
    onComplete(flattenedSelection);
  };

  const totalSelected = Object.values(selectedVariations).filter(
    Boolean
  ).length;

  return (
    <main className={`${styles.main} animate-in fade-in`}>
      <div className="mb-8 text-center">
        <p className={styles.eyebrow}>My Device</p>
        <h1 className={styles.sectionTitle}>Review Art</h1>
        <p className={styles.bodyText}>
          Select the versions you want to keep on Everframe.
        </p>
      </div>

      <div className={styles.reviewStack}>
        {items.map((item) => (
          <Card key={item.original.url} className={styles.reviewCard}>
            <CardContent className="p-4 sm:p-6">
              <div className={styles.reviewGrid}>
                <div className={styles.reviewFrame}>
                  <h3 className={styles.eyebrow}>Original</h3>
                  <div className={styles.reviewImage}>
                    <Image
                      src={item.original.url}
                      alt={item.original.name}
                      fill
                      className="object-cover"
                    />
                  </div>
                </div>

                <div className="group relative">
                  <label
                    htmlFor={`check-${item.original.url}`}
                    className="block h-full cursor-pointer"
                  >
                    <div className={styles.reviewFrame}>
                      <h3 className={styles.eyebrow}>
                        {item.variation.style}
                      </h3>
                      <div className={styles.reviewImage}>
                        <Image
                          src={item.variation.url}
                          alt={`Variation of ${item.original.name}`}
                          fill
                          className="object-cover transition-transform duration-300 group-hover:scale-105"
                        />
                        <div className="absolute inset-0 bg-black/50 opacity-0 transition-opacity group-hover:opacity-100" />
                      </div>
                    </div>
                    <div className="absolute top-6 right-6">
                      <Checkbox
                        id={`check-${item.original.url}`}
                        className="h-6 w-6 border-white bg-black/20 data-[state=checked]:bg-[#161818] data-[state=checked]:text-white"
                        checked={!!selectedVariations[item.original.url]}
                        onCheckedChange={(checked) =>
                          handleSelectionChange(
                            item.original.url,
                            item.variation.url,
                            !!checked
                          )
                        }
                      />
                    </div>
                  </label>
                </div>
              </div>
            </CardContent>
          </Card>
        ))}
      </div>

      <div className="sticky bottom-0 mt-8 flex justify-center py-4">
        <Button
          size="lg"
          onClick={handleFinalize}
          disabled={totalSelected === 0}
          className={styles.primaryButton}
        >
          <CheckCircle strokeWidth={1.5} />
          Keep {totalSelected > 0 ? `${totalSelected} ` : ''}Selected
          <ArrowRight className="ml-2" strokeWidth={1.5} />
        </Button>
      </div>
    </main>
  );
}
