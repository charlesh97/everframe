import { GoogleSignInButton } from '@/components/auth/login-form';
import { Toaster } from '@/components/ui/toaster';
import { FirebaseClientProvider } from '@/firebase';

export default function HomePage() {
  return (
    <FirebaseClientProvider>
      <main className="flex min-h-screen items-center justify-center bg-[#f3f0e4] px-6 py-12">
        <section className="w-full max-w-sm text-center" aria-labelledby="sign-in-title">
          <img
            src="/everframe/everframe-logo.svg"
            alt=""
            aria-hidden="true"
            className="mx-auto h-20 w-20"
          />

          <h1 id="sign-in-title" className="mt-8 text-4xl font-semibold tracking-tight text-[#3f3026]">
            Everframe
          </h1>
          <p className="mt-3 text-base leading-7 text-[#6f675f]">
            Sign in to manage your frame.
          </p>

          <div className="mt-8 rounded-2xl border border-black/10 bg-white/70 p-6 shadow-sm backdrop-blur-sm">
            <GoogleSignInButton />
          </div>

          <p className="mt-6 text-xs text-[#8b837a]">
            Your photos stay private to your account.
          </p>
        </section>
      </main>
      <Toaster />
    </FirebaseClientProvider>
  );
}
