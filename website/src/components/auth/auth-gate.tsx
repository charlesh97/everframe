'use client';

import { useEffect } from 'react';
import { useRouter } from 'next/navigation';
import { useUser } from '@/firebase';

export function AuthGate({ children }: { children: React.ReactNode }) {
  const { user, isUserLoading } = useUser();
  const router = useRouter();

  useEffect(() => {
    if (!isUserLoading && !user) {
      router.replace('/');
    }
  }, [isUserLoading, user, router]);

  if (isUserLoading) {
    return (
      <div className="flex min-h-[50vh] items-center justify-center p-8 text-sm text-muted-foreground">
        Checking sign-in…
      </div>
    );
  }

  if (!user) return null;

  return <>{children}</>;
}
