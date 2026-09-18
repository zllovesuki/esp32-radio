import { expect, test, vi } from 'vitest';
import { SfuClient } from '@/server/sfu.ts';

const credentials = { REALTIME_APP_ID: 'test-app', REALTIME_APP_TOKEN: 'test-token' };

test('cleanup accepts explicit absence at request and item level for tracks and channels', async () => {
  const sfu = new SfuClient(credentials);
  for (const resource of ['tracks', 'datachannels']) {
    for (const payload of [
      { errorCode: 'close_track_error' },
      { [resource === 'tracks' ? 'tracks' : 'dataChannels']: [{ errorCode: 'close_track_error' }] },
    ]) {
      vi.stubGlobal('fetch', async () => Response.json(payload));
      await sfu.call(`/sessions/old/${resource}/close`, {}, 'PUT');
    }
  }
});

test('cleanup keeps real failures pending beside already closed items', async () => {
  vi.spyOn(console, 'warn').mockImplementation(() => {});
  const sfu = new SfuClient(credentials);
  for (const resource of ['tracks', 'dataChannels']) {
    vi.stubGlobal('fetch', async () =>
      Response.json({
        errorCode: 'close_track_error',
        [resource]: [{ errorCode: 'close_track_error' }, { errorCode: 'backend_error' }],
      }),
    );
    await expect(
      sfu.call(`/sessions/old/${resource.toLowerCase()}/close`, {}, 'PUT'),
    ).rejects.toThrow(/could not configure/);
  }
});

test('absence handling does not hide failed HTTP requests or errors from other operations', async () => {
  vi.spyOn(console, 'warn').mockImplementation(() => {});
  const sfu = new SfuClient(credentials);
  for (const [operation, status, errorCode] of [
    ['tracks/new', 200, 'close_track_error'],
    ['datachannels/update', 200, 'close_track_error'],
    ['tracks/close', 503, 'close_track_error'],
    ['tracks/close', 200, 'backend_error'],
    ['tracks/close', 200, 'session_error'],
    ['tracks/close', 425, 'session_error'],
  ] as const) {
    vi.stubGlobal('fetch', async () => Response.json({ errorCode }, { status }));
    await expect(sfu.call(`/sessions/old/${operation}`, {}, 'PUT')).rejects.toThrow(
      /SFU operation failed/,
    );
  }
});
