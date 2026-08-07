'use client';

import { UploadCloud, GalleryHorizontal } from 'lucide-react';
import React, { useState } from 'react';
import { useDropzone } from 'react-dropzone';
import { Button } from '../ui/button';
import { cn } from '@/lib/utils';
import styles from './dashboard.module.css';

interface UploadViewProps {
  onUpload: (files: File[]) => void;
  onGoToGallery: () => void;
}

export default function UploadView({ onUpload, onGoToGallery }: UploadViewProps) {
  const [isDragging, setIsDragging] = useState(false);

  const { getRootProps, getInputProps, open } = useDropzone({
    onDrop: onUpload,
    accept: { 'image/*': ['.jpeg', '.png', '.gif', '.jpg'] },
    noClick: true,
    noKeyboard: true,
    onDragEnter: () => setIsDragging(true),
    onDragLeave: () => setIsDragging(false),
    onDropAccepted: () => setIsDragging(false),
  });

  return (
    <div className={cn(styles.centerStage, 'animate-in fade-in')}>
      <div
        {...getRootProps({
          className: cn(styles.heroPanel, styles.dropzone, isDragging && styles.dropzoneActive),
        })}
      >
        <input {...getInputProps()} />
        <div className="flex flex-col items-center gap-4">
          <div className={styles.iconBadge}>
            <UploadCloud
              className="h-7 w-7"
              strokeWidth={1.5}
            />
          </div>
          <p className={styles.eyebrow}>My Device</p>
          <h1 className={styles.title}>
            Add art to<br />
            Everframe.
          </h1>
          <p className={styles.bodyText}>
            Drop images here or choose files from your computer.
          </p>
          <div className={styles.actions}>
            <Button type="button" onClick={open} size="lg" className={styles.primaryButton}>
              Select Files
            </Button>
            <Button type="button" onClick={onGoToGallery} size="lg" variant="secondary" className={styles.secondaryButton}>
              <GalleryHorizontal strokeWidth={1.5} /> View Device
            </Button>
          </div>
        </div>
      </div>
    </div>
  );
}
