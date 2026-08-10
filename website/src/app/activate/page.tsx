'use client';

import { Suspense, useEffect, useState } from 'react';
import { useSearchParams, useRouter } from 'next/navigation';
import {
  collection,
  query,
  where,
  getDocs,
  doc,
  updateDoc,
  serverTimestamp,
} from 'firebase/firestore';

import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Card, CardContent, CardHeader, CardTitle, CardDescription } from '@/components/ui/card';
import { FirebaseClientProvider, useUser, useFirestore } from '@/firebase';
import { GoogleSignInButton } from '@/components/auth/login-form';
import Header from '@/components/layout/header';
import styles from '@/components/dashboard/dashboard.module.css';

function ActivateInner() {
  const searchParams = useSearchParams();
  const router = useRouter();
  const { user, isUserLoading } = useUser();
  const firestore = useFirestore();

  const initialCode = (searchParams.get('code') ?? '').toUpperCase().trim();
  const [code, setCode] = useState(initialCode);
  const [status, setStatus] = useState<'idle' | 'claiming' | 'success' | 'error'>('idle');
  const [message, setMessage] = useState<string>('');

  // Keep input in sync if the ?code= param changes without remount
  useEffect(() => {
    if (initialCode) setCode(initialCode);
  }, [initialCode]);

  const handleClaim = async () => {
    const normalized = code.trim().toUpperCase();
    if (!normalized) {
      setMessage('Enter the 6-character code from your frame.');
      setStatus('error');
      return;
    }
    if (!user) {
      setMessage('Sign in with Google first, then claim your frame.');
      setStatus('error');
      return;
    }
    if (!firestore) {
      setMessage('Firestore is not available. Refresh and try again.');
      setStatus('error');
      return;
    }

    setStatus('claiming');
    setMessage('');

    try {
      // Find a pending device with this activation code
      const q = query(
        collection(firestore, 'devices'),
        where('activationCode', '==', normalized),
        where('status', '==', 'pending'),
      );
      const snap = await getDocs(q);

      if (snap.empty) {
        setStatus('error');
        setMessage(
          'No pending device found for that code. Check the code on the frame or try again — it may already be claimed or expired.',
        );
        return;
      }

      if (snap.size > 1) {
        setStatus('error');
        setMessage('Multiple pending devices share that code. Contact support.');
        return;
      }

      const deviceDoc = snap.docs[0];
      const deviceRef = doc(firestore, 'devices', deviceDoc.id);

      await updateDoc(deviceRef, {
        userId: user.uid,
        status: 'claimed',
        claimedAt: serverTimestamp(),
      });

      setStatus('success');
      setMessage(`Frame ${normalized} linked to ${user.email ?? user.uid}.`);
      // Give the user a moment to see the success state, then go to dashboard
      setTimeout(() => router.push('/dashboard'), 1200);
    } catch (err: any) {
      console.error('Claim failed:', err);
      setStatus('error');
      const msg =
        err?.code === 'permission-denied'
          ? 'Permission denied — check Firestore rules for the devices claim transition.'
          : (err?.message ?? 'Could not claim the device. Please try again.');
      setMessage(msg);
    }
  };

  if (isUserLoading) {
    return (
      <div className={styles.appShell}>
        <div className="flex min-h-[50vh] items-center justify-center p-8 text-sm text-muted-foreground">
          Checking sign-in…
        </div>
      </div>
    );
  }

  // Not signed in yet — show Google button + code preview
  if (!user) {
    return (
      <div className={styles.appShell}>
        <div className="mx-auto max-w-md px-6 py-16">
          <Card>
            <CardHeader>
              <CardTitle>Link your EverFrame</CardTitle>
              <CardDescription>
                Sign in with Google, then enter the 6-character code from the frame.
              </CardDescription>
            </CardHeader>
            <CardContent className="space-y-4">
              <GoogleSignInButton />
              {code && (
                <p className="text-sm text-muted-foreground">
                  Code from QR: <span className="font-mono font-semibold">{code}</span>
                </p>
              )}
            </CardContent>
          </Card>
        </div>
      </div>
    );
  }

  return (
    <div className={styles.appShell}>
      <Header />
      <div className="mx-auto max-w-md px-6 py-10">
        <Card>
          <CardHeader>
            <CardTitle>Link your EverFrame</CardTitle>
            <CardDescription>
              Signed in as {user.email ?? user.uid}. Enter the code shown on the frame.
            </CardDescription>
          </CardHeader>
          <CardContent className="space-y-4">
            <div className="space-y-2">
              <label htmlFor="activationCode" className="text-sm font-medium">
                Activation code
              </label>
              <Input
                id="activationCode"
                value={code}
                onChange={(e) => setCode(e.target.value.toUpperCase())}
                placeholder="A7F3K9"
                maxLength={6}
                autoCapitalize="characters"
                autoComplete="off"
                spellCheck={false}
                className="font-mono tracking-widest"
              />
            </div>

            <Button onClick={handleClaim} disabled={status === 'claiming'} className="w-full" size="lg">
              {status === 'claiming' ? 'Linking…' : 'Link device'}
            </Button>

            {message && (
              <p
                className={
                  status === 'success' ? 'text-sm text-green-700' : 'text-sm text-destructive'
                }
                role={status === 'error' ? 'alert' : 'status'}
              >
                {message}
              </p>
            )}

            {status === 'success' && (
              <p className="text-sm text-muted-foreground">Redirecting to My Device…</p>
            )}
          </CardContent>
        </Card>
      </div>
    </div>
  );
}

export default function ActivatePage() {
  return (
    <FirebaseClientProvider>
      <Suspense
        fallback={
          <div className="flex min-h-[50vh] items-center justify-center p-8 text-sm text-muted-foreground">
            Loading…
          </div>
        }
      >
        <ActivateInner />
      </Suspense>
    </FirebaseClientProvider>
  );
}
