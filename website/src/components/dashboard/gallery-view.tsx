'use client';

import { Link2, PlusCircle, Send, Trash2, X } from 'lucide-react';
import Link from 'next/link';
import { useState, useEffect } from 'react';
import { collection, query, where } from 'firebase/firestore';
import { useCollection } from '@/firebase/firestore/use-collection';
import { useUser, useFirestore, useMemoFirebase } from '@/firebase';
import type { GalleryItem } from '@/app/dashboard/page';
import { Button } from '../ui/button';
import GalleryItemCard from './gallery-item';
import GenerationStyles from './generation-styles';
import DeviceStatus from './device-status';
import { Skeleton } from '../ui/skeleton';
import { AlertDialog, AlertDialogAction, AlertDialogCancel, AlertDialogContent, AlertDialogDescription, AlertDialogFooter, AlertDialogHeader, AlertDialogTitle } from '../ui/alert-dialog';
import styles from './dashboard.module.css';

interface GalleryViewProps {
  onUploadMore: () => void;
  onUpdateImage: (image: GalleryItem) => void;
  onDeleteImage: (image: GalleryItem) => void;
  onDeleteMany: (images: GalleryItem[]) => void;
  onSendImage: (image: GalleryItem) => Promise<void>;
}

export default function GalleryView({
  onUploadMore,
  onUpdateImage,
  onDeleteImage,
  onDeleteMany,
  onSendImage,
}: GalleryViewProps) {
  const { user } = useUser();
  const firestore = useFirestore();
  const [selectedImageIds, setSelectedImageIds] = useState<Set<string>>(new Set());
  const [isDeleteDialogOpen, setIsDeleteDialogOpen] = useState(false);
  const [isSending, setIsSending] = useState(false);
  const [statusRefreshKey, setStatusRefreshKey] = useState(0);
  const [currentSourcePath, setCurrentSourcePath] = useState<string | null>(null);

  const imagesCollection = useMemoFirebase(() => {
    if (!user) return null;
    return query(collection(firestore, 'images'), where('userId', '==', user.uid));
  }, [firestore, user]);

  const {
    data: images,
    isLoading,
    error,
  } = useCollection<GalleryItem>(imagesCollection);

  // Clear selection if the underlying image list changes
  useEffect(() => {
    setSelectedImageIds(new Set());
  }, [images]);

  const handleSelectionChange = (id: string, isSelected: boolean) => {
    setSelectedImageIds((prev) => {
      const newSet = new Set(prev);
      if (isSelected) {
        newSet.add(id);
      } else {
        newSet.delete(id);
      }
      return newSet;
    });
  };

  const handleDeleteSelected = () => {
    const imagesToDelete = images?.filter(img => selectedImageIds.has(img.id)) ?? [];
    onDeleteMany(imagesToDelete);
    setIsDeleteDialogOpen(false);
  };

  const numSelected = selectedImageIds.size;
  const selectedImage = numSelected === 1
    ? images?.find((image) => selectedImageIds.has(image.id))
    : undefined;

  const handleSendSelected = async () => {
    if (!selectedImage) return;
    setIsSending(true);
    try {
      await onSendImage(selectedImage);
      setStatusRefreshKey((value) => value + 1);
    } finally {
      setIsSending(false);
    }
  };

  const renderGalleryContent = () => {
    if (isLoading) {
      return (
        <div className={styles.galleryGrid}>
          {Array.from({ length: 5 }).map((_, i) => (
            <div key={i} className="flex flex-col space-y-3">
              <Skeleton className="h-[280px] w-full rounded" />
              <div className="space-y-2">
                <Skeleton className="h-4 w-4/5" />
                <Skeleton className="h-4 w-1/2" />
              </div>
            </div>
          ))}
        </div>
      );
    }

    if (error) {
      return (
        <div className={styles.errorState}>
          <h2 className="text-xl font-semibold text-destructive">
            Could not load My Device
          </h2>
          <p className={styles.bodyText}>
            Your artwork could not be fetched. Please try again later.
          </p>
          <pre className="mt-4 text-xs bg-muted p-2 rounded-md w-full max-w-lg overflow-auto">{error.message}</pre>
        </div>
      );
    }

    if (images && images.length > 0) {
      return (
        <div className={styles.galleryGrid}>
          {images.map((image) => (
            <GalleryItemCard
              key={image.id}
              item={image}
              isSelected={selectedImageIds.has(image.id)}
              onSelectionChange={handleSelectionChange}
              onUpdate={onUpdateImage}
              onDelete={onDeleteImage}
              isCurrent={image.storagePath === currentSourcePath}
            />
          ))}
        </div>
      );
    }

    return (
      <div className={styles.emptyState}>
        <h2 className={styles.eyebrow}>
          No Art Yet
        </h2>
        <p className={styles.bodyText}>
          Add your first image to begin filling Everframe.
        </p>
      </div>
    );
  };

  return (
    <>
      <main className={`${styles.main} animate-in fade-in`}>
        <div className={styles.toolbar}>
          <div>
            <p className={styles.eyebrow}>Everframe</p>
            <h1 className={styles.sectionTitle}>My Device</h1>
            <p className={styles.bodyText}>
              Choose what lives on your frame.
            </p>
          </div>
          <div className="flex flex-wrap gap-2">
            <GenerationStyles />
            <Button asChild variant="secondary" className={styles.secondaryButton}>
              <Link href="/activate">
                <Link2 strokeWidth={1.5} />
                Connect Device
              </Link>
            </Button>
            <Button onClick={onUploadMore} className={styles.primaryButton}>
              <PlusCircle strokeWidth={1.5} />
              Add Art
            </Button>
          </div>
        </div>

        <DeviceStatus
          refreshKey={statusRefreshKey}
          onCurrentPathChange={setCurrentSourcePath}
        />

        {numSelected > 0 && (
          <div className={styles.selectionBanner}>
            <p className="text-sm font-semibold">{numSelected} selected</p>
            <div className="flex items-center gap-2">
              <Button
                variant="secondary"
                size="sm"
                disabled={!selectedImage || isSending}
                onClick={handleSendSelected}
                className="rounded-full"
              >
                <Send strokeWidth={1.5} />
                {isSending ? 'Queuing…' : numSelected === 1 ? 'Send to frame' : 'Select one to send'}
              </Button>
              <Button variant="destructive" size="sm" onClick={() => setIsDeleteDialogOpen(true)} className="rounded-full">
                <Trash2 strokeWidth={1.5} /> Delete
              </Button>
              <Button variant="ghost" size="icon" onClick={() => setSelectedImageIds(new Set())} aria-label="Clear selection" className="h-9 w-9 rounded-full text-white hover:bg-white/10 hover:text-white">
                <X strokeWidth={1.5} />
              </Button>
            </div>
          </div>
        )}

        {renderGalleryContent()}
      </main>

      <AlertDialog open={isDeleteDialogOpen} onOpenChange={setIsDeleteDialogOpen}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>Are you sure?</AlertDialogTitle>
            <AlertDialogDescription>
              This action cannot be undone. This will permanently delete the
              {numSelected} selected image(s) from My Device and storage.
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>Cancel</AlertDialogCancel>
            <AlertDialogAction
              onClick={handleDeleteSelected}
              className="bg-destructive text-destructive-foreground hover:bg-destructive/90"
            >
              Delete
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </>
  );
}
