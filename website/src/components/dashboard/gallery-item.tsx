'use client';

import { zodResolver } from '@hookform/resolvers/zod';
import { MoreVertical, Pencil, Trash2 } from 'lucide-react';
import Image from 'next/image';
import { useState } from 'react';
import { useForm } from 'react-hook-form';
import * as z from 'zod';

import type { GalleryItem } from '@/app/dashboard/page';
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
import { Button } from '@/components/ui/button';
import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu';
import {
  Form,
  FormControl,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { useToast } from '@/hooks/use-toast';
import { Card, CardFooter } from '../ui/card';
import { cn } from '@/lib/utils';
import { Checkbox } from '../ui/checkbox';
import styles from './dashboard.module.css';

interface GalleryItemCardProps {
  item: GalleryItem;
  isSelected: boolean;
  onSelectionChange: (id: string, selected: boolean) => void;
  onUpdate: (item: GalleryItem) => void;
  onDelete: (item: GalleryItem) => void;
}

const renameFormSchema = z.object({
  name: z.string().min(1, { message: 'Name cannot be empty.' }),
});

export default function GalleryItemCard({
  item,
  isSelected,
  onSelectionChange,
  onUpdate,
  onDelete,
}: GalleryItemCardProps) {
  const [isRenameOpen, setIsRenameOpen] = useState(false);
  const [isDeleteOpen, setIsDeleteOpen] = useState(false);
  const { toast } = useToast();

  const form = useForm<z.infer<typeof renameFormSchema>>({
    resolver: zodResolver(renameFormSchema),
    defaultValues: { name: item.name },
  });

  const handleToggleDisplay = (isDisplayed: boolean) => {
    onUpdate({ ...item, isDisplayed });
  };

  const handleRenameSubmit = (values: z.infer<typeof renameFormSchema>) => {
    onUpdate({ ...item, name: values.name });
    toast({
      title: 'Image Renamed',
      description: `"${item.name}" was renamed to "${values.name}".`,
    });
    setIsRenameOpen(false);
  };

  const handleDeleteConfirm = () => {
    onDelete(item); // Pass the full item to the handler
    setIsDeleteOpen(false);
  };

  return (
    <>
      <Card
        className={cn(
          'group relative flex flex-col',
          styles.artCard,
          isSelected && styles.artCardSelected
        )}
      >
        <div className="relative aspect-[3/4] w-full">
          <Image
            src={item.url}
            alt={item.name}
            fill
            className={styles.artImage}
            data-ai-hint="abstract art"
            sizes="(max-width: 768px) 100vw, (max-width: 1200px) 50vw, 33vw"
            onClick={() => onSelectionChange(item.id, !isSelected)}
          />
           <div className="absolute top-2 left-2 z-10">
            <Checkbox
              checked={isSelected}
              onCheckedChange={(checked) => onSelectionChange(item.id, !!checked)}
              className={cn("h-6 w-6 border-white bg-black/20 data-[state=checked]:bg-[#161818] data-[state=checked]:text-white transition-opacity",
                isSelected ? "opacity-100" : "opacity-0 group-hover:opacity-100"
              )}
              aria-label={`Select image ${item.name}`}
            />
          </div>
          <div className="absolute top-2 right-2">
            <DropdownMenu>
              <DropdownMenuTrigger asChild>
                <Button
                  variant="secondary"
                  size="icon"
                  className="h-8 w-8 rounded-full bg-[#f1efe9]/90 opacity-0 transition-opacity hover:bg-white group-hover:opacity-100"
                >
                  <MoreVertical className="h-4 w-4" strokeWidth={1.5} />
                </Button>
              </DropdownMenuTrigger>
              <DropdownMenuContent align="end">
                <DropdownMenuItem onClick={() => setIsRenameOpen(true)} disabled>
                  <Pencil className="mr-2" strokeWidth={1.5} />
                  Rename (soon)
                </DropdownMenuItem>
                <DropdownMenuItem
                  onClick={() => setIsDeleteOpen(true)}
                  className="text-destructive focus:bg-destructive/10 focus:text-destructive"
                >
                  <Trash2 className="mr-2" strokeWidth={1.5} />
                  Delete
                </DropdownMenuItem>
              </DropdownMenuContent>
            </DropdownMenu>
          </div>
        </div>
        <CardFooter className={styles.cardFooter}>
          <p
            className={styles.cardName}
            title={item.name}
          >
            {item.name}
          </p>
          <div className={styles.cardMetaRow}>
            <Label htmlFor={`display-${item.id}`} className="text-sm text-muted-foreground">
              On frame
            </Label>
            <Switch
              id={`display-${item.id}`}
              checked={item.isDisplayed}
              onCheckedChange={handleToggleDisplay}
            />
          </div>
        </CardFooter>
      </Card>

      {/* Rename Dialog - Disabled for now */}
      <Dialog open={isRenameOpen} onOpenChange={setIsRenameOpen}>
        <DialogContent>
          <DialogHeader>
            <DialogTitle>Rename Image</DialogTitle>
          </DialogHeader>
          <Form {...form}>
            <form
              onSubmit={form.handleSubmit(handleRenameSubmit)}
              className="space-y-4"
            >
              <FormField
                control={form.control}
                name="name"
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>New Name</FormLabel>
                    <FormControl>
                      <Input {...field} />
                    </FormControl>
                    <FormMessage />
                  </FormItem>
                )}
              />
              <DialogFooter>
                <Button type="submit">Save Changes</Button>
              </DialogFooter>
            </form>
          </Form>
        </DialogContent>
      </Dialog>

      {/* Delete Confirmation Dialog */}
      <AlertDialog open={isDeleteOpen} onOpenChange={setIsDeleteOpen}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>Are you sure?</AlertDialogTitle>
            <AlertDialogDescription>
              This action cannot be undone. This will permanently delete the
              image "{item.name}" from My Device and storage.
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>Cancel</AlertDialogCancel>
            <AlertDialogAction
              onClick={handleDeleteConfirm}
              className="bg-destructive text-destructive-foreground hover:bg-destructive/90"
            >
              Delete
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </>
  );
}
