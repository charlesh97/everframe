'use client';

import { GoogleAuthProvider, signInWithPopup } from 'firebase/auth';
import { useRouter } from 'next/navigation';

import { Button } from '@/components/ui/button';
import { useAuth } from '@/firebase';
import { useToast } from '@/hooks/use-toast';

export function GoogleSignInButton() {
  const router = useRouter();
  const auth = useAuth();
  const { toast } = useToast();

  const handleGoogleSignIn = async () => {
    const provider = new GoogleAuthProvider();
    // Force account picker when multiple Google accounts are signed in.
    provider.setCustomParameters({ prompt: 'select_account' });
    try {
      await signInWithPopup(auth, provider);
      router.push('/dashboard');
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
    <Button onClick={handleGoogleSignIn} className="w-full !mt-8" size="lg">
      Continue with Google
    </Button>
  );
}

// Keep the old export name as an alias so existing imports don't break
// during the transition. New code should import GoogleSignInButton.
export const EmailSignInForm = GoogleSignInButton;
