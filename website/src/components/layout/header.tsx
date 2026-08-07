'use client';

import { LogOut } from 'lucide-react';
import Link from 'next/link';
import { useRouter } from 'next/navigation';
import { useAuth } from '@/firebase';
import { Button } from '@/components/ui/button';
import styles from '@/components/dashboard/dashboard.module.css';

interface HeaderProps {
  onLogoClick?: () => void;
}

export default function Header({ onLogoClick }: HeaderProps) {
  const router = useRouter();
  const auth = useAuth();

  const handleLogout = async () => {
    await auth.signOut();
    router.push('/');
  };

  return (
    <header className={styles.header}>
      <button onClick={onLogoClick} className={styles.wordmark} type="button" aria-label="Open My Device">
        <span className={styles.wordmarkMark} aria-hidden="true" />
        Everframe
      </button>

      <nav className={styles.navigation} aria-label="Primary navigation">
        <Link href="/" className={styles.navLink}>
          Everframe
        </Link>
        <Link href="/dashboard" className={styles.activeNavLink} aria-current="page">
          My Device
        </Link>
      </nav>

      <Button variant="ghost" size="sm" onClick={handleLogout} className={styles.headerAction}>
        Sign Out <LogOut aria-hidden="true" size={14} strokeWidth={1.5} />
      </Button>
    </header>
  );
}
