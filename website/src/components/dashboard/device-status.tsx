'use client';

import { CircleAlert, Link2, LoaderCircle, MonitorCheck, Radio, Unlink, WifiOff } from 'lucide-react';
import Link from 'next/link';
import { useCallback, useEffect, useState } from 'react';

import { Button } from '@/components/ui/button';
import { useUser } from '@/firebase';
import { useToast } from '@/hooks/use-toast';
import styles from './dashboard.module.css';

interface DeviceSummary {
  id: string;
  state: 'linked' | 'queued' | 'processing' | 'ready' | 'displayed' | 'waiting_for_photo' | 'error';
  online: boolean;
  awake: boolean;
  lastSeen: string | null;
  displayedHash: string | null;
  lastError: string | null;
  schedule: 'manual' | '12h' | 'daily' | 'weekly';
  scheduleVersion: string;
  appliedScheduleVersion: string | null;
  schedulePending: boolean;
  nextWakeAt: string | null;
}

interface DeviceStatusResponse {
  devices: DeviceSummary[];
  current: {
    hash: string | null;
    sourcePath: string | null;
    createdAt: string | null;
  } | null;
}

interface DeviceStatusProps {
  refreshKey: number;
  onCurrentPathChange?: (path: string | null) => void;
}

function statusCopy(device: DeviceSummary) {
  if (device.state === 'error') return device.lastError ?? 'The last refresh failed. Everframe will retry.';
  if (device.state === 'queued') return device.awake ? 'Photo queued. The frame is awake.' : 'Photo queued. Press the frame button to send it now.';
  if (device.state === 'processing') return 'Preparing your photo for the display.';
  if (device.state === 'ready') return device.awake ? 'Photo is ready and the frame is checking now.' : 'Photo is ready. Press the frame button to receive it now.';
  if (device.state === 'displayed') return device.awake ? 'Current photo displayed. The frame is awake.' : 'Your frame is up to date.';
  if (device.state === 'waiting_for_photo') return 'Linked and waiting for its first photo.';
  return 'Linked and ready for a photo.';
}

export default function DeviceStatus({ refreshKey, onCurrentPathChange }: DeviceStatusProps) {
  const { user } = useUser();
  const { toast } = useToast();
  const [data, setData] = useState<DeviceStatusResponse | null>(null);
  const [error, setError] = useState(false);
  const [removingId, setRemovingId] = useState<string | null>(null);
  const [savingScheduleId, setSavingScheduleId] = useState<string | null>(null);

  const loadStatus = useCallback(async () => {
    if (!user) return;
    try {
      const token = await user.getIdToken();
      const endpoint = process.env.NEXT_PUBLIC_DEVICE_STATUS_URL
        ?? 'https://us-central1-everframe-479214.cloudfunctions.net/deviceStatus';
      const response = await fetch(endpoint, {
        headers: { Authorization: `Bearer ${token}` },
        cache: 'no-store',
      });
      if (!response.ok) throw new Error(`device status returned HTTP ${response.status}`);
      const nextData = await response.json() as DeviceStatusResponse;
      setData(nextData);
      setError(false);
      onCurrentPathChange?.(nextData.current?.sourcePath ?? null);
    } catch (statusError) {
      console.error('Could not load device status:', statusError);
      setError(true);
    }
  }, [onCurrentPathChange, user]);

  useEffect(() => {
    void loadStatus();
    const timer = window.setInterval(() => void loadStatus(), 10_000);
    return () => window.clearInterval(timer);
  }, [loadStatus, refreshKey]);

  const removeDevice = async (device: DeviceSummary) => {
    if (!user || removingId) return;
    const shortId = device.id.slice(-6).toUpperCase();
    if (!window.confirm(
      `Remove Everframe ${shortId} from your account? Your photos will stay in your gallery. `
      + 'The frame will show its pairing code the next time it wakes.',
    )) return;

    setRemovingId(device.id);
    try {
      const token = await user.getIdToken();
      const endpoint = process.env.NEXT_PUBLIC_REMOVE_DEVICE_URL
        ?? 'https://us-central1-everframe-479214.cloudfunctions.net/removeDevice';
      const response = await fetch(endpoint, {
        method: 'POST',
        headers: {
          Authorization: `Bearer ${token}`,
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ deviceId: device.id }),
      });
      const result: unknown = await response.json().catch(() => ({}));
      if (!response.ok) {
        const message = typeof result === 'object' && result !== null && 'error' in result
          && typeof result.error === 'string'
          ? result.error
          : `remove device returned HTTP ${response.status}`;
        throw new Error(message);
      }
      await loadStatus();
      toast({
        title: 'Frame removed',
        description: 'It can be paired again with the code shown on the frame.',
      });
    } catch (removeError) {
      console.error('Could not remove device:', removeError);
      toast({
        title: 'Could not remove frame',
        description: removeError instanceof Error ? removeError.message : 'Please try again.',
        variant: 'destructive',
      });
    } finally {
      setRemovingId(null);
    }
  };

  const updateSchedule = async (device: DeviceSummary, schedule: DeviceSummary['schedule']) => {
    if (!user || savingScheduleId) return;
    setSavingScheduleId(device.id);
    try {
      const token = await user.getIdToken();
      const endpoint = process.env.NEXT_PUBLIC_UPDATE_DEVICE_SCHEDULE_URL
        ?? 'https://us-central1-everframe-479214.cloudfunctions.net/updateDeviceSchedule';
      const response = await fetch(endpoint, {
        method: 'POST',
        headers: {
          Authorization: `Bearer ${token}`,
          'Content-Type': 'application/json',
        },
        body: JSON.stringify({ deviceId: device.id, schedule }),
      });
      const result: unknown = await response.json().catch(() => ({}));
      if (!response.ok) {
        const message = typeof result === 'object' && result !== null && 'error' in result
          && typeof result.error === 'string'
          ? result.error
          : `schedule update returned HTTP ${response.status}`;
        throw new Error(message);
      }
      await loadStatus();
      toast({
        title: 'Schedule saved',
        description: device.awake
          ? 'The frame will apply it during this wake window.'
          : 'Press the frame button to apply it now, or wait for its next check.',
      });
    } catch (scheduleError) {
      console.error('Could not update frame schedule:', scheduleError);
      toast({
        title: 'Could not update schedule',
        description: scheduleError instanceof Error ? scheduleError.message : 'Please try again.',
        variant: 'destructive',
      });
    } finally {
      setSavingScheduleId(null);
    }
  };

  if (error && !data) {
    return (
      <section className={styles.deviceStatusCard}>
        <CircleAlert />
        <div><strong>Status unavailable</strong><p>Photos can still be saved. Try again shortly.</p></div>
      </section>
    );
  }

  const devices = data?.devices ?? [];
  if (devices.length === 0) {
    return (
      <section className={styles.deviceStatusCard}>
        <Link2 />
        <div><strong>No frame linked</strong><p>Connect an Everframe before sending photos.</p></div>
        <Button asChild variant="secondary"><Link href="/activate">Connect</Link></Button>
      </section>
    );
  }

  return (
    <div className={styles.deviceStatusList} aria-live="polite">
      {devices.map((device) => {
        const StatusIcon = device.state === 'processing'
          ? LoaderCircle
          : device.state === 'error'
            ? CircleAlert
            : device.online || device.awake
              ? Radio
              : device.state === 'displayed'
                ? MonitorCheck
                : WifiOff;
        return (
          <section className={styles.deviceStatusCard} key={device.id}>
            <StatusIcon className={device.state === 'processing' ? styles.statusSpinner : undefined} />
            <div>
              <div className={styles.deviceStatusHeading}>
                <strong>Everframe {device.id.slice(-6).toUpperCase()}</strong>
                <span className={device.online || device.awake ? styles.statusOnline : styles.statusSleeping}>
                  {device.online || device.awake ? 'Awake' : 'Sleeping'}
                </span>
              </div>
              <p>{statusCopy(device)}</p>
              <div className={styles.deviceScheduleControl}>
                <label htmlFor={`schedule-${device.id}`}>Update schedule</label>
                <select
                  id={`schedule-${device.id}`}
                  value={device.schedule}
                  disabled={savingScheduleId !== null}
                  onChange={(event) => void updateSchedule(
                    device,
                    event.target.value as DeviceSummary['schedule'],
                  )}
                >
                  <option value="manual">Manual only</option>
                  <option value="12h">12 hours</option>
                  <option value="daily">Daily</option>
                  <option value="weekly">Weekly</option>
                </select>
                <small>
                  {device.schedulePending
                    ? 'Pending on frame — press its button to apply now.'
                    : device.nextWakeAt
                      ? `Next check ${new Date(device.nextWakeAt).toLocaleString()}`
                      : 'Timer wake is off; use the frame button.'}
                </small>
              </div>
            </div>
            <Button
              type="button"
              variant="outline"
              size="sm"
              disabled={removingId !== null}
              onClick={() => void removeDevice(device)}
            >
              {removingId === device.id ? <LoaderCircle className={styles.statusSpinner} /> : <Unlink />}
              {removingId === device.id ? 'Removing…' : 'Remove'}
            </Button>
          </section>
        );
      })}
      <Button asChild variant="ghost" size="sm" className={styles.addDeviceButton}>
        <Link href="/activate"><Link2 />Add another frame</Link>
      </Button>
    </div>
  );
}
