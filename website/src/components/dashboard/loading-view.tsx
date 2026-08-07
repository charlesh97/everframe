import styles from './dashboard.module.css';

interface LoadingViewProps {
  title: string;
  description: string;
}

export default function LoadingView({ title, description }: LoadingViewProps) {
  return (
    <div className={`${styles.centerStage} animate-in fade-in`}>
      <div className="flex flex-col items-center gap-4">
        <div className={styles.loadingSpinner} />
        <h2 className={styles.sectionTitle}>
          {title}
        </h2>
        <p className={styles.bodyText}>
          {description}
        </p>
      </div>
    </div>
  );
}
