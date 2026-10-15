import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise';
import type { Db } from '../db/pool.js';

export interface Ban {
  id: number;
  accountId: number;
  reason: string;
  expiresAt: Date | null;
  bannedBy: number | null;
  createdAt: Date;
}

const ACTIVE = 'account_id = ? AND lifted_at IS NULL AND (expires_at IS NULL OR expires_at > ?)';

export async function findActiveBan(db: Db, accountId: number, now: Date): Promise<Ban | undefined> {
  const [rows] = await db.query<RowDataPacket[]>(
    `SELECT id, account_id, reason, expires_at, banned_by, created_at FROM bans WHERE ${ACTIVE}
      ORDER BY created_at DESC, id DESC LIMIT 1`,
    [accountId, now],
  );
  const r = rows[0];
  return (
    r && {
      id: r.id,
      accountId: r.account_id,
      reason: r.reason,
      expiresAt: r.expires_at,
      bannedBy: r.banned_by,
      createdAt: r.created_at,
    }
  );
}

export async function liftActiveBans(db: Db, accountId: number, liftedBy: number, now: Date): Promise<void> {
  await db.query(`UPDATE bans SET lifted_at = ?, lifted_by = ? WHERE ${ACTIVE}`, [now, liftedBy, accountId, now]);
}

export async function insertBan(db: Db, ban: Omit<Ban, 'id'>): Promise<Ban> {
  const [res] = await db.query<ResultSetHeader>(
    'INSERT INTO bans (account_id, reason, expires_at, banned_by, created_at) VALUES (?, ?, ?, ?, ?)',
    [ban.accountId, ban.reason, ban.expiresAt, ban.bannedBy, ban.createdAt],
  );
  return { id: res.insertId, ...ban };
}
