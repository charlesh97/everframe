'use client';

import { UploadCloud, GalleryHorizontal } from 'lucide-react';
import React, { useState } from 'react';
import { useDropzone } from 'react-dropzone';
import { Button } from '../ui/button';
import { cn } from '@/lib/utils';
import { MAX_IMPORT_BYTES } from '@/lib/image-import';
import styles from './dashboard.module.css';

interface UploadViewProps {
  onUpload: (files: File[]) => void;
  onGoToGallery: () => void;
  onError: (message: string) => void;
}

export default function UploadView({ onUpload, onGoToGallery, onError }: UploadViewProps) {
  const [isDragging, setIsDragging] = useState(false);

  const { getRootProps, getInputProps, open } = useDropzone({
    onDropAccepted: (files) => {
      setIsDragging(false);
      onUpload(files);
    },
    onDropRejected: (rejections) => {
      setIsDragging(false);
      const firstError = rejections[0]?.errors[0];
      onError(
        firstError?.code === 'file-too-large'
          ? 'Each photo must be 75 MB or smaller.'
          : 'Choose JPEG, PNG, HEIC, or HEIF photos only.'
      );
    },
    accept: {
      'image/jpeg': ['.jpg', '.jpeg'],
      'image/png': ['.png'],
      'image/heic': ['.heic'],
      'image/heif': ['.heif'],
    },
    maxSize: MAX_IMPORT_BYTES,
    noClick: true,
    noKeyboard: true,
    onDragEnter: () => setIsDragging(true),
    onDragLeave: () => setIsDragging(false),
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
            Choose photos, fit each one to the Everframe display, then upload.
          </p>
          <p className={styles.fileHint}>JPEG, PNG, HEIC or HEIF · up to 75 MB each</p>
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
