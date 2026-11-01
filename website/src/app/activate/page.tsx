'use client';

import { Suspense, useEffect, useState } from 'react';
import { useSearchParams, useRouter } from 'next/navigation';

import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Card, CardContent, CardHeader, CardTitle, CardDescription } from '@/components/ui/card';
import { FirebaseClientProvider, useUser } from '@/firebase';
import { GoogleSignInButton } from '@/components/auth/login-form';
import Header from '@/components/layout/header';
import styles from '@/components/dashboard/dashboard.module.css';

function ActivateInner() {
  const searchParams = useSearchParams();
  const router = useRouter();
  const { user, isUserLoading } = useUser();

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
    setStatus('claiming');
    setMessage('');

    try {
      const token = await user.getIdToken();
      const endpoint = process.env.NEXT_PUBLIC_CLAIM_DEVICE_URL
        ?? 'https://us-central1-everframe-479214.cloudfunctions.net/claimDevice';
      const response = await fetch(endpoint, {
        method: 'POST',
        headers: {
          Authorization: `Bearer ${token}`,
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ activationCode: normalized }),
      });
      const result: unknown = await response.json().catch(() => ({}));
      if (!response.ok) {
        const errorMessage = typeof result === 'object'
          && result !== null
          && 'error' in result
          && typeof result.error === 'string'
          ? result.error
          : 'Could not claim the device.';
        throw new Error(errorMessage);
      }

      const refreshQueued = typeof result === 'object'
        && result !== null
        && 'refreshQueued' in result
        && result.refreshQueued === true;

      setStatus('success');
      setMessage(
        refreshQueued
          ? `Frame ${normalized} linked to ${user.email ?? user.uid}. Its first refresh is queued.`
          : `Frame ${normalized} linked. It will refresh on the next scheduled sync.`,
      );
      // Give the user a moment to see the success state, then go to dashboard
      setTimeout(() => router.push('/dashboard'), 1200);
    } catch (err: any) {
      console.error('Claim failed:', err);
      setStatus('error');
      const msg = err?.message ?? 'Could not claim the device. Please try again.';
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
              <GoogleSignInButton
                redirectTo={`/activate${code ? `?code=${encodeURIComponent(code)}` : ''}`}
              />
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
      <Header
        onLogoClick={() => router.push('/dashboard?view=device')}
        onAddPhotos={() => router.push('/dashboard')}
        onMyDevice={() => router.push('/dashboard?view=device')}
        activeSection="device"
      />
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
