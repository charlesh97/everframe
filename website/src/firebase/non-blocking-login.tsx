'use client';
import {
  Auth,
  GoogleAuthProvider,
  signInWithPopup,
} from 'firebase/auth';

/** Initiate Google sign-in (non-blocking helper for callers that don't want to await). */
export function initiateGoogleSignIn(authInstance: Auth): void {
  const provider = new GoogleAuthProvider();
  provider.setCustomParameters({ prompt: 'select_account' });
  signInWithPopup(authInstance, provider).catch((err: any) => {
    if (err?.code === 'auth/popup-closed-by-user' || err?.code === 'auth/cancelled-popup-request') return;
    console.error('Google sign-in error:', err);
  });
}

// ---------------------------------------------------------------------------
// Legacy helpers retained so old call sites still type-check during migration.
// New code should use initiateGoogleSignIn.
// ---------------------------------------------------------------------------
import {
  signInAnonymously as _signInAnonymously,
  createUserWithEmailAndPassword as _createUserWithEmailAndPassword,
  signInWithEmailAndPassword as _signInWithEmailAndPassword,
} from 'firebase/auth';

/** @deprecated Use initiateGoogleSignIn */
export function initiateAnonymousSignIn(authInstance: Auth): void {
  _signInAnonymously(authInstance).catch((err: any) => {
    console.error('Anonymous sign-in error:', err);
  });
}

/** @deprecated Use initiateGoogleSignIn */
export function initiateEmailSignUp(authInstance: Auth, email: string, password: string): void {
  _createUserWithEmailAndPassword(authInstance, email, password).catch((err: any) => {
    console.error('Email sign-up error:', err);
  });
}

/** @deprecated Use initiateGoogleSignIn */
export function initiateEmailSignIn(authInstance: Auth, email: string, password: string): void {
  _signInWithEmailAndPassword(authInstance, email, password).catch((err: any) => {
    console.error('Email sign-in error:', err);
  });
}
