import { Toaster } from '@/components/ui/toaster';
import { FirebaseClientProvider } from '@/firebase';
import { AuthGate } from '@/components/auth/auth-gate';

export default function DashboardLayout({
  children,
}: {
  children: React.ReactNode;
}) {
  return (
    <FirebaseClientProvider>
      <AuthGate>{children}</AuthGate>
      <Toaster />
    </FirebaseClientProvider>
  );
}
