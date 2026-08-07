import type { Metadata } from 'next';
import './globals.css';

export const metadata: Metadata = {
  title: 'Everframe',
  description: 'Art that never gets old.',
};

export default function RootLayout({
  children,
}: Readonly<{
  children: React.ReactNode;
}>) {
  return (
    <html lang="en" className="h-full">
      <head>
        <link rel="preconnect" href="https://fonts.googleapis.com" />
        <link rel="preconnect" href="https://fonts.gstatic.com" crossOrigin="anonymous" />
        <link href="https://fonts.googleapis.com/css2?family=Bebas+Neue&display=swap" rel="stylesheet" />
      </head>
      <body className="font-body min-h-full antialiased">
        {children}
      </body>
    </html>
  );
}
