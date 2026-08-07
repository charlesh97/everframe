import Link from 'next/link';
import { ArrowUpRight } from 'lucide-react';
import type { RefObject } from 'react';
import styles from './everframe-hero.module.css';

interface EverframeHeaderProps {
  headerRef: RefObject<HTMLElement>;
}

export function EverframeHeader({ headerRef }: EverframeHeaderProps) {
  return (
    <header ref={headerRef} className={styles.header}>
      <Link href="/" className={styles.wordmark} aria-label="Everframe home">
        <span className={styles.wordmarkMark} aria-hidden="true" />
        Everframe
      </Link>

      <nav className={styles.navigation} aria-label="Primary navigation">
        <Link href="/" className={styles.activeNavLink} aria-current="page">
          Everframe
        </Link>
        <Link href="/dashboard" className={styles.navLink}>
          My Device
        </Link>
      </nav>

      <a
        className={styles.reserveLink}
        href="https://github.com/charlesh97/everframe/"
        target="_blank"
        rel="noreferrer"
      >
        GitHub <ArrowUpRight aria-hidden="true" size={14} strokeWidth={1.5} />
      </a>
    </header>
  );
}
