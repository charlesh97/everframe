'use client';

import { Palette, Pencil, Plus, Sparkles, Trash2 } from 'lucide-react';
import { useEffect, useRef, useState } from 'react';
import {
  collection,
  deleteDoc,
  doc,
  orderBy,
  query,
  serverTimestamp,
  setDoc,
  updateDoc,
  writeBatch,
} from 'firebase/firestore';

import { useDoc, useFirestore, useMemoFirebase, useUser } from '@/firebase';
import { useCollection } from '@/firebase/firestore/use-collection';
import { useToast } from '@/hooks/use-toast';
import { Button } from '@/components/ui/button';
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog';
import {
  Dialog,
  DialogContent,
  DialogDescription,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { Input } from '@/components/ui/input';
import { Textarea } from '@/components/ui/textarea';
import {
  EVERFRAME_GENERATION_STYLE_COUNT,
  MAX_GENERATION_STYLES,
  STARTER_GENERATION_STYLES,
} from '@/lib/generation-styles';
import styles from './dashboard.module.css';

const MAX_LABEL_LENGTH = 60;
const MAX_PROMPT_LENGTH = 160;

export interface GenerationStyle {
  id: string;
  userId: string;
  label: string;
  prompt: string;
}

interface StyleLibraryProfile {
  generationStylesInitialized?: boolean;
}

/**
 * A small, optional starter set copied into each owner's private library. The
 * processor still owns the base instruction and uses `prompt` as its style
 * clause, so it cannot be replaced with an arbitrary image-generation prompt.
 */
function normalise(value: string) {
  return value.trim().replace(/\s+/g, ' ');
}

function styleError(label: string, prompt: string) {
  if (!label) return 'Give this style a short name.';
  if (label.length > MAX_LABEL_LENGTH) return `Style names can be at most ${MAX_LABEL_LENGTH} characters.`;
  if (!prompt) return 'Describe the visual style to use.';
  if (prompt.length > MAX_PROMPT_LENGTH) return `Style prompts can be at most ${MAX_PROMPT_LENGTH} characters.`;
  return null;
}

export default function GenerationStyles() {
  const { user } = useUser();
  const firestore = useFirestore();
  const { toast } = useToast();
  const [isOpen, setIsOpen] = useState(false);
  const [editing, setEditing] = useState<GenerationStyle | null>(null);
  const [label, setLabel] = useState('');
  const [prompt, setPrompt] = useState('');
  const [isSaving, setIsSaving] = useState(false);
  const [isClearOpen, setIsClearOpen] = useState(false);
  const initializationAttempted = useRef(false);

  const stylesQuery = useMemoFirebase(() => {
    if (!user) return null;
    return query(
      collection(firestore, 'users', user.uid, 'generationStyles'),
      orderBy('createdAt', 'asc')
    );
  }, [firestore, user]);
  const { data: generationStyles, isLoading, error } = useCollection<GenerationStyle>(stylesQuery);
  const savedStyles = generationStyles ?? [];
  const userDocRef = useMemoFirebase(
    () => user ? doc(firestore, 'users', user.uid) : null,
    [firestore, user]
  );
  const { data: styleProfile, isLoading: isProfileLoading } = useDoc<StyleLibraryProfile>(userDocRef);

  useEffect(() => {
    if (
      !isOpen || !user || !userDocRef || isLoading || isProfileLoading || error ||
      styleProfile?.generationStylesInitialized || initializationAttempted.current
    ) return;

    initializationAttempted.current = true;
    const initialiseLibrary = async () => {
      setIsSaving(true);
      try {
        const batch = writeBatch(firestore);
        if (savedStyles.length === 0) {
          STARTER_GENERATION_STYLES.forEach(([starterLabel, starterPrompt]) => {
            const styleRef = doc(collection(firestore, 'users', user.uid, 'generationStyles'));
            batch.set(styleRef, {
              userId: user.uid,
              label: starterLabel,
              prompt: starterPrompt,
              createdAt: serverTimestamp(),
              updatedAt: serverTimestamp(),
            });
          });
        }
        batch.set(userDocRef, {
          id: user.uid,
          generationStylesInitialized: true,
        }, { merge: true });
        await batch.commit();
      } catch (initialiseError) {
        initializationAttempted.current = false;
        console.error('Could not initialise generation styles:', initialiseError);
        toast({ title: 'Could not load EverFrame styles', description: 'Please reopen this panel.', variant: 'destructive' });
      } finally {
        setIsSaving(false);
      }
    };
    void initialiseLibrary();
  }, [error, firestore, isLoading, isOpen, isProfileLoading, savedStyles.length, styleProfile?.generationStylesInitialized, toast, user, userDocRef]);

  const resetEditor = () => {
    setEditing(null);
    setLabel('');
    setPrompt('');
  };

  const beginEdit = (style: GenerationStyle) => {
    setEditing(style);
    setLabel(style.label);
    setPrompt(style.prompt);
  };

  const saveStyle = async () => {
    if (!user || !firestore) return;
    const nextLabel = normalise(label);
    const nextPrompt = normalise(prompt);
    const validationError = styleError(nextLabel, nextPrompt);
    if (validationError) {
      toast({ title: 'Check this style', description: validationError, variant: 'destructive' });
      return;
    }

    if (!editing && savedStyles.length >= MAX_GENERATION_STYLES) {
      toast({
        title: 'Style limit reached',
        description: `Keep up to ${MAX_GENERATION_STYLES} styles for now. Delete one to add another.`,
        variant: 'destructive',
      });
      return;
    }

    setIsSaving(true);
    try {
      if (editing) {
        await updateDoc(doc(firestore, 'users', user.uid, 'generationStyles', editing.id), {
          label: nextLabel,
          prompt: nextPrompt,
          updatedAt: serverTimestamp(),
        });
        toast({ title: 'Style updated' });
      } else {
        const styleRef = doc(collection(firestore, 'users', user.uid, 'generationStyles'));
        await setDoc(styleRef, {
          userId: user.uid,
          label: nextLabel,
          prompt: nextPrompt,
          createdAt: serverTimestamp(),
          updatedAt: serverTimestamp(),
        });
        toast({ title: 'Style saved' });
      }
      resetEditor();
    } catch (saveError) {
      console.error('Could not save generation style:', saveError);
      toast({ title: 'Could not save style', description: 'Please try again.', variant: 'destructive' });
    } finally {
      setIsSaving(false);
    }
  };

  const clearAllStyles = async () => {
    if (!user || !firestore) return;
    setIsSaving(true);
    try {
      const batch = writeBatch(firestore);
      savedStyles.forEach((style) => {
        batch.delete(doc(firestore, 'users', user.uid, 'generationStyles', style.id));
      });
      await batch.commit();
      resetEditor();
      setIsClearOpen(false);
      toast({ title: 'All styles cleared', description: 'Restore the EverFrame styles whenever you want.' });
    } catch (clearError) {
      console.error('Could not clear generation styles:', clearError);
      toast({ title: 'Could not clear styles', description: 'Please try again.', variant: 'destructive' });
    } finally {
      setIsSaving(false);
    }
  };

  const restoreEverFrameStyles = async () => {
    if (!user || !firestore) return;
    const existingPrompts = new Set(savedStyles.map((style) => normalise(style.prompt).toLowerCase()));
    const stylesToAdd = STARTER_GENERATION_STYLES
      .filter(([, starterPrompt]) => !existingPrompts.has(starterPrompt.toLowerCase()))
      .slice(0, MAX_GENERATION_STYLES - savedStyles.length);
    if (stylesToAdd.length === 0) {
      toast({ title: 'EverFrame styles are already restored' });
      return;
    }
    setIsSaving(true);
    try {
      const batch = writeBatch(firestore);
      stylesToAdd.forEach(([starterLabel, starterPrompt]) => {
        const styleRef = doc(collection(firestore, 'users', user.uid, 'generationStyles'));
        batch.set(styleRef, {
          userId: user.uid,
          label: starterLabel,
          prompt: starterPrompt,
          createdAt: serverTimestamp(),
          updatedAt: serverTimestamp(),
        });
      });
      await batch.commit();
      toast({ title: 'EverFrame styles restored', description: `${stylesToAdd.length} styles added.` });
    } catch (restoreError) {
      console.error('Could not restore EverFrame styles:', restoreError);
      toast({ title: 'Could not restore styles', description: 'Please try again.', variant: 'destructive' });
    } finally {
      setIsSaving(false);
    }
  };

  const removeStyle = async (style: GenerationStyle) => {
    if (!user || !firestore) return;
    try {
      await deleteDoc(doc(firestore, 'users', user.uid, 'generationStyles', style.id));
      if (editing?.id === style.id) resetEditor();
      toast({ title: 'Style deleted' });
    } catch (deleteError) {
      console.error('Could not delete generation style:', deleteError);
      toast({ title: 'Could not delete style', description: 'Please try again.', variant: 'destructive' });
    }
  };

  const editorTitle = editing ? 'Edit style' : 'Add a style';

  return (
    <>
      <Button
        type="button"
        variant="secondary"
        className={styles.secondaryButton}
        onClick={() => setIsOpen(true)}
      >
        <Palette strokeWidth={1.5} /> Art Styles
      </Button>

      <Dialog open={isOpen} onOpenChange={(open) => {
        setIsOpen(open);
        if (!open) resetEditor();
      }}>
        <DialogContent className={styles.stylesDialog}>
          <DialogHeader>
            <DialogTitle>Art styles</DialogTitle>
            <DialogDescription>
              Your library starts with 100 editable EverFrame styles. Keep up to 200 total.
            </DialogDescription>
          </DialogHeader>

          <div className={styles.stylesNotice}>
            <Sparkles strokeWidth={1.5} />
            <p>Styles describe the look only. Everframe keeps the photo and frame composition instruction fixed.</p>
          </div>

          <div className={styles.styleEditor}>
            <div className={styles.styleEditorHeading}>
              <h3>{editorTitle}</h3>
              {editing && (
                <Button type="button" variant="ghost" size="sm" onClick={resetEditor}>
                  Cancel edit
                </Button>
              )}
            </div>
            <label className={styles.styleField}>
              <span>Name</span>
              <Input
                value={label}
                maxLength={MAX_LABEL_LENGTH}
                placeholder="e.g. Sunday watercolor"
                onChange={(event) => setLabel(event.target.value)}
              />
            </label>
            <label className={styles.styleField}>
              <span>Style prompt</span>
              <Textarea
                value={prompt}
                maxLength={MAX_PROMPT_LENGTH}
                placeholder="e.g. luminous watercolor painting on textured paper"
                onChange={(event) => setPrompt(event.target.value)}
              />
              <small>{prompt.length}/{MAX_PROMPT_LENGTH}</small>
            </label>
            <Button type="button" className={styles.primaryButton} disabled={isSaving} onClick={saveStyle}>
              {editing ? <Pencil strokeWidth={1.5} /> : <Plus strokeWidth={1.5} />}
              {editing ? 'Save changes' : 'Save style'}
            </Button>
          </div>

          <div className={styles.savedStyles}>
            <div className={styles.styleEditorHeading}>
              <h3>Style library</h3>
              <span>{savedStyles.length}/{MAX_GENERATION_STYLES}</span>
              {savedStyles.length > 0 && (
                <Button type="button" variant="ghost" size="sm" onClick={() => setIsClearOpen(true)}>
                  Clear all
                </Button>
              )}
            </div>
            {isLoading ? (
              <p className={styles.bodyText}>Loading styles…</p>
            ) : error ? (
              <p className={styles.styleError}>Could not load your styles. Please reopen this panel.</p>
            ) : savedStyles.length === 0 ? (
              <div className={styles.emptyStyles}>
                <p>No styles saved. New frames use the original photo without AI restyling.</p>
                <Button type="button" variant="secondary" disabled={isSaving} onClick={restoreEverFrameStyles}>
                  Restore {EVERFRAME_GENERATION_STYLE_COUNT} EverFrame styles
                </Button>
              </div>
            ) : (
              <ul className={styles.styleList}>
                {savedStyles.map((style) => (
                  <li key={style.id}>
                    <div>
                      <strong>{style.label}</strong>
                      <p>{style.prompt}</p>
                    </div>
                    <div className={styles.styleRowActions}>
                      <Button type="button" variant="ghost" size="icon" aria-label={`Edit ${style.label}`} onClick={() => beginEdit(style)}>
                        <Pencil strokeWidth={1.5} />
                      </Button>
                      <Button type="button" variant="ghost" size="icon" aria-label={`Delete ${style.label}`} onClick={() => removeStyle(style)}>
                        <Trash2 strokeWidth={1.5} />
                      </Button>
                    </div>
                  </li>
                ))}
              </ul>
            )}
          </div>

          <DialogFooter>
            <Button type="button" variant="ghost" onClick={() => setIsOpen(false)}>Done</Button>
          </DialogFooter>
        </DialogContent>
      </Dialog>

      <AlertDialog open={isClearOpen} onOpenChange={setIsClearOpen}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>Clear all styles?</AlertDialogTitle>
            <AlertDialogDescription>
              Are you sure? This permanently removes all {savedStyles.length} styles, including the EverFrame starter styles. New frames will use the original photo without AI restyling until you add or restore styles.
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel disabled={isSaving}>Cancel</AlertDialogCancel>
            <AlertDialogAction disabled={isSaving} onClick={clearAllStyles}>
              Clear all styles
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </>
  );
}
