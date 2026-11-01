'use client';

import { GoogleAuthProvider, signInWithPopup } from 'firebase/auth';
import { useRouter } from 'next/navigation';

import { Button } from '@/components/ui/button';
import { useAuth } from '@/firebase';
import { useToast } from '@/hooks/use-toast';

export function GoogleSignInButton({ redirectTo = '/dashboard' }: { redirectTo?: string }) {
  const router = useRouter();
  const auth = useAuth();
  const { toast } = useToast();

  const handleGoogleSignIn = async () => {
    const provider = new GoogleAuthProvider();
    // Force account picker when multiple Google accounts are signed in.
    provider.setCustomParameters({ prompt: 'select_account' });
    try {
      await signInWithPopup(auth, provider);
      router.push(redirectTo);
    } catch (error: any) {
      // User closed the popup — not an error to surface.
      if (error?.code === 'auth/popup-closed-by-user' || error?.code === 'auth/cancelled-popup-request') {
        return;
      }
      console.error('Google sign-in error:', error);
      toast({
        title: 'Sign-in failed',
        description: error?.message ?? 'Could not sign in with Google. Please try again.',
        variant: 'destructive',
      });
    }
  };

  return (
    <Button
      onClick={handleGoogleSignIn}
      className="h-12 w-full rounded-xl bg-[#3f3026] text-base text-white shadow-none hover:bg-[#2f241d]"
      size="lg"
    >
      <svg viewBox="0 0 24 24" aria-hidden="true" className="h-5 w-5">
        <path
          fill="currentColor"
          d="M21.6 12.23c0-.71-.06-1.4-.18-2.07H12v3.92h5.38a4.6 4.6 0 0 1-2 3.02v2.54h3.24c1.9-1.75 2.98-4.33 2.98-7.41Z"
        />
        <path
          fill="currentColor"
          fillOpacity=".78"
          d="M12 22c2.7 0 4.98-.9 6.63-2.36l-3.24-2.54c-.9.6-2.05.96-3.39.96-2.61 0-4.82-1.76-5.61-4.13H3.04v2.62A10 10 0 0 0 12 22Z"
        />
        <path
          fill="currentColor"
          fillOpacity=".58"
          d="M6.39 13.93A6 6 0 0 1 6.08 12c0-.67.11-1.32.31-1.93V7.45H3.04A10 10 0 0 0 2 12c0 1.61.39 3.14 1.04 4.55l3.35-2.62Z"
        />
        <path
          fill="currentColor"
          fillOpacity=".88"
          d="M12 5.94c1.47 0 2.79.51 3.83 1.5l2.87-2.88A9.63 9.63 0 0 0 12 2a10 10 0 0 0-8.96 5.45l3.35 2.62C7.18 7.7 9.39 5.94 12 5.94Z"
        />
      </svg>
      Continue with Google
    </Button>
  );
}

// Keep the old export name as an alias so existing imports don't break
// during the transition. New code should import GoogleSignInButton.
export const EmailSignInForm = GoogleSignInButton;
