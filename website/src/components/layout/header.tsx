'use client';

import { LogOut } from 'lucide-react';
import { useRouter } from 'next/navigation';
import { useAuth } from '@/firebase';
import { Button } from '@/components/ui/button';
import styles from '@/components/dashboard/dashboard.module.css';

interface HeaderProps {
  onLogoClick?: () => void;
  onAddPhotos: () => void;
  onMyDevice: () => void;
  activeSection: 'photos' | 'device';
  navigationDisabled?: boolean;
}

export default function Header({
  onLogoClick,
  onAddPhotos,
  onMyDevice,
  activeSection,
  navigationDisabled = false,
}: HeaderProps) {
  const router = useRouter();
  const auth = useAuth();

  const handleLogout = async () => {
    await auth.signOut();
    router.replace('/');
  };

  return (
    <header className={styles.header}>
      <button onClick={onLogoClick} className={styles.wordmark} type="button" aria-label="Open My Device">
        <span className={styles.wordmarkMark} aria-hidden="true" />
        Everframe
      </button>

      <nav className={styles.navigation} aria-label="Primary navigation">
        <button
          type="button"
          onClick={onAddPhotos}
          disabled={navigationDisabled}
          className={activeSection === 'photos' ? styles.activeNavLink : styles.navLink}
          aria-current={activeSection === 'photos' ? 'page' : undefined}
        >
          Add Photos
        </button>
        <button
          type="button"
          onClick={onMyDevice}
          disabled={navigationDisabled}
          className={activeSection === 'device' ? styles.activeNavLink : styles.navLink}
          aria-current={activeSection === 'device' ? 'page' : undefined}
        >
          My Device
        </button>
      </nav>

      <Button variant="ghost" size="sm" onClick={handleLogout} className={styles.headerAction}>
        Sign Out <LogOut aria-hidden="true" size={14} strokeWidth={1.5} />
      </Button>
    </header>
  );
}
