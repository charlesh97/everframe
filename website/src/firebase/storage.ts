'use client';

import { getStorage, ref as storageRef, FirebaseStorage } from 'firebase/storage';
import { useFirebaseApp } from './provider';

/**
 * Returns a hook that provides the Firebase Storage instance.
 */
export function useStorage(): FirebaseStorage {
    const app = useFirebaseApp();
    return getStorage(app);
}

/**
 * Returns a storage reference to a specific file path in the root of the bucket.
 * @param filePath The path to the file (e.g., 'images/art.jpg').
 * @returns A storage reference to the specified path.
 */
export function getFileRef(filePath: string) {
    return storageRef(getStorage(), filePath);
}
