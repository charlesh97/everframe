import { cn } from "@/lib/utils";

const Logo = ({ className }: { className?: string }) => (
  <svg
    xmlns="http://www.w3.org/2000/svg"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    strokeWidth="2"
    strokeLinecap="round"
    strokeLinejoin="round"
    className={cn("text-primary", className)}
  >
    <rect width="18" height="18" x="3" y="3" rx="2" />
    <path d="M3 10h18" />
    <path d="M10 3v18" />
  </svg>
);

export default Logo;
