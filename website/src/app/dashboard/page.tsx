'use client';

import { useEffect, useState } from 'react';
import { useUser, useFirestore } from '@/firebase';
import {
  collection,
  doc,
  setDoc,
  updateDoc,
  deleteDoc,
  serverTimestamp,
} from 'firebase/firestore';
import {
  getStorage,
  ref as storageRef,
  uploadBytes,
  getDownloadURL,
  deleteObject,
} from 'firebase/storage';

import GalleryView from '@/components/dashboard/gallery-view';
import LoadingView from '@/components/dashboard/loading-view';
import ReviewView from '@/components/dashboard/review-view';
import UploadView from '@/components/dashboard/upload-view';
import Header from '@/components/layout/header';
import { useToast } from '@/hooks/use-toast';
import {
  cropImageToJpeg,
  prepareImportFile,
  releaseImportItems,
  type CropSelection,
  type ImportItem,
} from '@/lib/image-import';
import styles from '@/components/dashboard/dashboard.module.css';

export interface GalleryItem {
  id: string;
  url: string;
  name: string;
  isDisplayed: boolean;
  storagePath: string; // Ensure this is present
  originalImage?: string; // This will be a data URI for preview, not stored in Firestore
}

export type AppState = 'UPLOAD' | 'PREPARING' | 'LOADING_SAVE' | 'REVIEW' | 'GALLERY';

interface RefreshQueueResult {
  queued: boolean;
  replaced: boolean;
}

export default function DashboardPage() {
  const [appState, setAppState] = useState<AppState>('UPLOAD');
  const [reviewItems, setReviewItems] = useState<ImportItem[]>([]);
  const { toast } = useToast();
  const { user } = useUser();
  const firestore = useFirestore();

  const queueDeviceRefresh = async (imagePath: string): Promise<RefreshQueueResult> => {
    if (!user) throw new Error('You must be signed in to send a photo.');
    const token = await user.getIdToken();
    const endpoint = process.env.NEXT_PUBLIC_REFRESH_DEVICE_URL
      ?? 'https://us-central1-everframe-479214.cloudfunctions.net/refreshDevice';
    const response = await fetch(endpoint, {
      method: 'POST',
      headers: {
        Authorization: `Bearer ${token}`,
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({ imagePath }),
    });
    const result: unknown = await response.json().catch(() => ({}));
    if (!response.ok) {
      const message = typeof result === 'object' && result !== null && 'error' in result
        && typeof result.error === 'string'
        ? result.error
        : `refresh queue returned HTTP ${response.status}`;
      throw new Error(message);
    }
    return {
      queued: true,
      replaced: typeof result === 'object' && result !== null && 'replaced' in result
        && result.replaced === true,
    };
  };

  useEffect(() => {
    if (new URLSearchParams(window.location.search).get('view') === 'device') {
      setAppState('GALLERY');
      window.history.replaceState(null, '', '/dashboard');
    }
  }, []);

  const handleFilesUpload = async (files: File[]) => {
    setAppState('PREPARING');
    const preparedItems: ImportItem[] = [];
    try {
      for (const file of files) {
        preparedItems.push(await prepareImportFile(file));
      }
      setReviewItems(preparedItems);
      setAppState('REVIEW');
    } catch (error) {
      releaseImportItems(preparedItems);
      console.error('Error preparing images:', error);
      toast({
        title: 'Could not open photo',
        description: error instanceof Error ? error.message : 'The selected photo could not be prepared.',
        variant: 'destructive',
      });
      setAppState('UPLOAD');
    }
  };

  const handleReviewComplete = async (selectedImages: CropSelection[]) => {
    if (!user || !firestore) {
      toast({
        title: 'Authentication Error',
        description: 'You must be logged in to save images.',
        variant: 'destructive',
      });
      return;
    }
    setAppState('LOADING_SAVE');

    const storage = getStorage();
    const imagesCollectionRef = collection(firestore, 'images');
    let lastUploadedPath = '';
    let refreshQueued = true;
    let refreshReplaced = false;

    try {
      // Process sequentially to avoid holding several full-resolution canvases
      // in memory at once on phones. Stable IDs make a retry idempotent.
      for (const item of selectedImages) {
        const croppedImage = await cropImageToJpeg(item.item.previewUrl, item.cropPixels);
        const baseName = item.item.name.replace(/\.[^.]+$/, '').replace(/[^a-zA-Z0-9_-]+/g, '-');
        const imageName = `${baseName || 'photo'}-${item.item.id}`;
        const storagePath = `users/${user.uid}/images/${imageName}.jpg`;
        const imageRef = storageRef(storage, storagePath);

        await uploadBytes(imageRef, croppedImage, {
          contentType: 'image/jpeg',
          customMetadata: {
            sourceType: item.item.sourceType,
            cropAspect: '3:5',
            cropVersion: '1',
          },
        });
        const downloadURL = await getDownloadURL(imageRef);

        await setDoc(doc(imagesCollectionRef, item.item.id), {
          name: imageName,
          storagePath,
          url: downloadURL,
          isDisplayed: true,
          createdAt: serverTimestamp(),
          userId: user.uid,
          sourceType: item.item.sourceType,
          crop: {
              aspect: '3:5',
              outputWidth: 960,
              outputHeight: 1600,
          },
        });
        lastUploadedPath = storagePath;
      }

      if (lastUploadedPath) {
        try {
          const queued = await queueDeviceRefresh(lastUploadedPath);
          refreshReplaced = queued.replaced;
        } catch (refreshError) {
          console.error('Image saved but device refresh could not be queued:', refreshError);
          refreshQueued = false;
        }
      }

      toast({
        title: 'Success!',
        description: refreshQueued
          ? refreshReplaced
            ? `${selectedImages.length} new image(s) saved. The previous pending frame request was replaced.`
            : `${selectedImages.length} new image(s) saved. Your frame refresh is queued.`
          : `${selectedImages.length} new image(s) saved. The frame will use the next scheduled sync.`,
      });
      releaseImportItems(reviewItems);
      setReviewItems([]);
      setAppState('GALLERY');
    } catch (error) {
      console.error('Error saving images:', error);
      toast({
        title: 'Error Saving Images',
        description:
          'There was a problem saving your images to My Device. Please try again.',
        variant: 'destructive',
      });
      setAppState('REVIEW');
    }
  };

  const handleImportError = (message: string) => {
    toast({ title: 'Unsupported photo', description: message, variant: 'destructive' });
  };

  const handleCancelReview = () => {
    releaseImportItems(reviewItems);
    setReviewItems([]);
    setAppState('UPLOAD');
  };

  const handleUpdateImage = async (updatedImage: GalleryItem) => {
    if (!user || !firestore) return;
    const imageDocRef = doc(firestore, 'images', updatedImage.id);
    try {
      await updateDoc(imageDocRef, { name: updatedImage.name, isDisplayed: updatedImage.isDisplayed });
    } catch (error) {
      console.error('Failed to update image:', error);
      toast({
        title: 'Update Failed',
        description: 'Could not save your change. Please try again.',
        variant: 'destructive',
      });
    }
  };

  const handleDeleteImage = async (image: GalleryItem) => {
    if (!user || !firestore) return;
    const storage = getStorage();
    try {
      // Correctly reference the file in Storage using its full path, then delete
      const imageFileRef = storageRef(storage, image.storagePath);
      await deleteObject(imageFileRef);

      // Delete the document from Firestore
      await deleteDoc(doc(firestore, 'images', image.id));
    } catch (error) {
      console.error(`Failed to delete image ${image.id}:`, error);
      throw error; // Re-throw to be caught by the bulk handler
    }
  };

  const handleDeleteMany = async (images: GalleryItem[]) => {
    if (images.length === 0) return;

    let successCount = 0;
    let failureCount = 0;

    await Promise.all(images.map(async (image) => {
      try {
        await handleDeleteImage(image);
        successCount++;
      } catch (e) {
        console.error(`Failed to delete image ${image.id} during bulk delete:`, e);
        failureCount++;
      }
    }))

    if (failureCount > 0) {
      toast({
        title: 'Deletion Error',
        description: `Could not delete ${failureCount} image(s). Please try again.`,
        variant: 'destructive',
      });
    }

    if (successCount > 0) {
      toast({
        title: 'Images Deleted',
        description: `${successCount} image(s) were removed from My Device.`,
      });
    }
  };


  const handleUploadMore = () => {
    releaseImportItems(reviewItems);
    setReviewItems([]);
    setAppState('UPLOAD');
  };

  const handleGoToGallery = () => {
    releaseImportItems(reviewItems);
    setReviewItems([]);
    setAppState('GALLERY');
  };

  const handleSendImage = async (image: GalleryItem) => {
    try {
      const queued = await queueDeviceRefresh(image.storagePath);
      toast({
        title: queued.replaced ? 'Pending photo replaced' : 'Photo queued',
        description: queued.replaced
          ? 'Only this newer selection can be published to your frame.'
          : 'If your frame is sleeping, press its button once. It will listen for five minutes.',
      });
    } catch (error) {
      console.error('Could not send image to frame:', error);
      toast({
        title: 'Could not queue photo',
        description: error instanceof Error ? error.message : 'Please try again.',
        variant: 'destructive',
      });
      throw error;
    }
  };

  const renderContent = () => {
    switch (appState) {
      case 'UPLOAD':
        return (
          <UploadView
            onUpload={handleFilesUpload}
            onGoToGallery={handleGoToGallery}
            onError={handleImportError}
          />
        );
      case 'PREPARING':
        return (
          <LoadingView
            title="Opening Photos"
            description="Preparing your photos for cropping."
          />
        );
      case 'LOADING_SAVE':
        return (
          <LoadingView
            title="Updating Device"
            description="Selected artwork is being saved to My Device."
          />
        );
      case 'REVIEW':
        return (
          <ReviewView
            items={reviewItems}
            onComplete={handleReviewComplete}
            onCancel={handleCancelReview}
          />
        );
      case 'GALLERY':
        return (
          <GalleryView
            onUploadMore={handleUploadMore}
            onUpdateImage={handleUpdateImage}
            onDeleteImage={handleDeleteImage}
            onDeleteMany={handleDeleteMany}
            onSendImage={handleSendImage}
          />
        );
      default:
        return null;
    }
  };

  return (
    <div className={styles.appShell}>
      <Header
        onLogoClick={handleGoToGallery}
        onAddPhotos={handleUploadMore}
        onMyDevice={handleGoToGallery}
        activeSection={appState === 'GALLERY' ? 'device' : 'photos'}
        navigationDisabled={appState === 'PREPARING' || appState === 'LOADING_SAVE'}
      />
      {renderContent()}
    </div>
  );
}
