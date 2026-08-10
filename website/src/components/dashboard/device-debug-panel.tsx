'use client';

import { useState } from 'react';
import { doc, getDoc } from 'firebase/firestore';
import { Button } from '@/components/ui/button';
import { Card, CardContent, CardHeader, CardTitle, CardDescription } from '@/components/ui/card';
import { useFirestore } from '@/firebase';

interface DeviceStatus {
  deviceId: string;
  status: string;
  activationCode?: string;
  userId?: string;
  lastImageHash?: string;
}

/**
 * Debug panel for /dashboard — lets you look up a device doc you own.
 * Not linked from nav; add it to the dashboard when you want it.
 * Uses the signed-in user's Firestore read (get requires owner), so it
 * doubles as a rules sanity check without touching device secrets.
 */
export function DeviceDebugPanel() {
  const firestore = useFirestore();
  const [deviceId, setDeviceId] = useState('');
  const [result, setResult] = useState<DeviceStatus | null>(null);
  const [error, setError] = useState<string>('');

  const lookup = async () => {
    setError('');
    setResult(null);
    const id = deviceId.trim().toLowerCase();
    if (!id) {
      setError('Enter a deviceId');
      return;
    }
    try {
      const snap = await getDoc(doc(firestore, 'devices', id));
      if (!snap.exists()) {
        setError('No such device (or not yet registered).');
        return;
      }
      setResult({ deviceId: snap.id, ...(snap.data() as Omit<DeviceStatus, 'deviceId'>) });
    } catch (e: any) {
      setError(e?.message ?? 'Lookup failed — check Firestore rules (owner only).');
    }
  };

  return (
    <Card>
      <CardHeader>
        <CardTitle className="text-base">Device debug</CardTitle>
        <CardDescription>Look up a device you own by deviceId (owner-only read).</CardDescription>
      </CardHeader>
      <CardContent className="space-y-3">
        <div className="flex gap-2">
          <input
            value={deviceId}
            onChange={(e) => setDeviceId(e.target.value)}
            placeholder="a3f9… 32 hex chars"
            className="flex-1 rounded-md border px-3 py-2 text-sm font-mono"
            spellCheck={false}
            autoComplete="off"
          />
          <Button onClick={lookup} size="sm">Lookup</Button>
        </div>
        {error && <p className="text-sm text-destructive" role="alert">{error}</p>}
        {result && (
          <pre className="rounded bg-muted p-3 text-xs overflow-auto">
            {JSON.stringify(result, null, 2)}
          </pre>
        )}
      </CardContent>
    </Card>
  );
}
