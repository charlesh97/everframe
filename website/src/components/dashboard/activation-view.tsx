'use client';

import { zodResolver } from '@hookform/resolvers/zod';
import { KeyRound } from 'lucide-react';
import { useForm } from 'react-hook-form';
import * as z from 'zod';

import { Button } from '@/components/ui/button';
import {
  Card,
  CardContent,
  CardDescription,
  CardFooter,
  CardHeader,
  CardTitle,
} from '@/components/ui/card';
import {
  Form,
  FormControl,
  FormField,
  FormItem,
  FormLabel,
  FormMessage,
} from '@/components/ui/form';
import { Input } from '@/components/ui/input';

const formSchema = z.object({
  activationCode: z
    .string()
    .min(6, { message: 'Code must be at least 6 characters.' })
    .max(6, { message: 'Code must be 6 characters.' }),
});

interface ActivationViewProps {
  onActivate: () => void;
}

export default function ActivationView({ onActivate }: ActivationViewProps) {
  const form = useForm<z.infer<typeof formSchema>>({
    resolver: zodResolver(formSchema),
    defaultValues: {
      activationCode: '',
    },
  });

  function onSubmit() {
    // In a real app, you would verify the activation code here.
    onActivate();
  }

  return (
    <div className="container mx-auto flex h-full animate-in fade-in items-center justify-center p-4">
      <Card className="w-full max-w-md shadow-lg">
        <Form {...form}>
          <form onSubmit={form.handleSubmit(onSubmit)}>
            <CardHeader>
              <CardTitle className="font-headline text-2xl">
                Activate Your Everframe
              </CardTitle>
              <CardDescription>
                Enter the 6-digit code displayed on your device screen to link it
                to your account.
              </CardDescription>
            </CardHeader>
            <CardContent>
              <FormField
                control={form.control}
                name="activationCode"
                render={({ field }) => (
                  <FormItem>
                    <FormLabel>Activation Code</FormLabel>
                    <FormControl>
                      <Input
                        placeholder="_ _ _ _ _ _"
                        {...field}
                        className="text-center text-lg tracking-[0.5em]"
                      />
                    </FormControl>
                    <FormMessage />
                  </FormItem>
                )}
              />
            </CardContent>
            <CardFooter>
              <Button type="submit" className="w-full">
                <KeyRound /> Activate Device
              </Button>
            </CardFooter>
          </form>
        </Form>
      </Card>
    </div>
  );
}
