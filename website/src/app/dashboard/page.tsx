'use client';

import { useState } from 'react';
import { useUser, useFirestore } from '@/firebase';
import {
  collection,
  doc,
  addDoc,
  updateDoc,
  deleteDoc,
  serverTimestamp,
} from 'firebase/firestore';
import {
  getStorage,
  ref as storageRef,
  uploadString,
  getDownloadURL,
  deleteObject,
} from 'firebase/storage';

import { transformImageToAbstractArt } from '@/ai/flows/transform-image-to-abstract-art';
import GalleryView from '@/components/dashboard/gallery-view';
import LoadingView from '@/components/dashboard/loading-view';
import ReviewView from '@/components/dashboard/review-view';
import UploadView from '@/components/dashboard/upload-view';
import Header from '@/components/layout/header';
import { useToast } from '@/hooks/use-toast';
import styles from '@/components/dashboard/dashboard.module.css';

export interface GalleryItem {
  id: string;
  url: string;
  name: string;
  isDisplayed: boolean;
  storagePath: string; // Ensure this is present
  originalImage?: string; // This will be a data URI for preview, not stored in Firestore
}

export interface ReviewItem {
  original: {
    name: string;
    url: string; // data URI
  };
  variation: {
    url: string; // data URI
    style: string;
  };
}

export type AppState = 'UPLOAD' | 'LOADING_AI' | 'LOADING_SAVE' | 'REVIEW' | 'GALLERY';

const fileToDataUri = (file: File) =>
  new Promise<string>((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = (event) => {
      resolve(event.target?.result as string);
    };
    reader.onerror = reject;
    reader.readAsDataURL(file);
  });

export default function DashboardPage() {
  const [appState, setAppState] = useState<AppState>('UPLOAD');
  const [reviewItems, setReviewItems] = useState<ReviewItem[]>([]);
  const { toast } = useToast();
  const { user } = useUser();
  const firestore = useFirestore();

  const handleFilesUpload = async (files: File[]) => {
    setAppState('LOADING_AI');
    try {
      const imageProcessingPromises = files.map(async (file) => {
        const photoDataUri = await fileToDataUri(file);
        const result = await transformImageToAbstractArt({ photoDataUri });
        return {
          original: { name: file.name, url: photoDataUri },
          variation: { url: result.abstractArtUrl, style: result.style },
        };
      });

      const newReviewItems = await Promise.all(imageProcessingPromises);
      setReviewItems(newReviewItems);
      setAppState('REVIEW');
    } catch (error) {
      console.error('Error transforming images:', error);
      toast({
        title: 'Transformation Error',
        description:
          'There was a problem generating abstract art. Please try again.',
        variant: 'destructive',
      });
      setAppState('UPLOAD'); // Revert to upload state on error
    }
  };

  const handleReviewComplete = async (
    selectedImages: { original: ReviewItem['original']; variationUrl: string }[]
  ) => {
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

    try {
      await Promise.all(
        selectedImages.map(async (item) => {
          const imageName = `${item.original.name.split('.')[0]}-art-${Date.now()}`;
          const storagePath = `users/${user.uid}/images/${imageName}.png`;
          const imageRef = storageRef(storage, storagePath);

          // Upload the base64 data URI to Firebase Storage
          await uploadString(imageRef, item.variationUrl, 'data_url');
          const downloadURL = await getDownloadURL(imageRef);

          // Save the metadata to Firestore
          await addDoc(imagesCollectionRef, {
            name: imageName,
            storagePath: storagePath, // Make sure this is being saved
            url: downloadURL,
            isDisplayed: true,
            createdAt: serverTimestamp(),
            userId: user.uid,
          });
        })
      );

      toast({
        title: 'Success!',
        description: `${selectedImages.length} new image(s) have been saved to My Device.`,
      });
    } catch (error) {
      console.error('Error saving images:', error);
      toast({
        title: 'Error Saving Images',
        description:
          'There was a problem saving your images to My Device. Please try again.',
        variant: 'destructive',
      });
    } finally {
      setReviewItems([]);
      setAppState('GALLERY');
    }
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
    setAppState('UPLOAD');
  };

  const handleGoToGallery = () => {
    setAppState('GALLERY');
  };

  const handleLogoClick = () => {
    setAppState('GALLERY');
  };

  const renderContent = () => {
    switch (appState) {
      case 'UPLOAD':
        return (
          <UploadView
            onUpload={handleFilesUpload}
            onGoToGallery={handleGoToGallery}
          />
        );
      case 'LOADING_AI':
        return (
          <LoadingView
            title="Preparing Art"
            description="Your image is being shaped for Everframe."
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
          <ReviewView items={reviewItems} onComplete={handleReviewComplete} />
        );
      case 'GALLERY':
        return (
          <GalleryView
            onUploadMore={handleUploadMore}
            onUpdateImage={handleUpdateImage}
            onDeleteImage={handleDeleteImage}
            onDeleteMany={handleDeleteMany}
          />
        );
      default:
        return null;
    }
  };

  return (
    <div className={styles.appShell}>
      <Header onLogoClick={handleLogoClick} />
      {renderContent()}
    </div>
  );
}
