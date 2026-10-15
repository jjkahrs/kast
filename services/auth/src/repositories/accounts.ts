import type { ResultSetHeader, RowDataPacket } from 'mysql2/promise';
import type { Db } from '../db/pool.js';

export const ROLES = ['player', 'gm', 'admin'] as const;
export type Role = (typeof ROLES)[number];

export interface Account {
  id: number;
  email: string;
  passwordHash: string;
  role: Role;
  emailVerifiedAt: Date | null;
  failedLoginCount: number;
  lockedUntil: Date | null;
  createdAt: Date;
}

const toAccount = (r: RowDataPacket): Account => ({
  id: r.id,
  email: r.email,
  passwordHash: r.password_hash,
  role: r.role,
  emailVerifiedAt: r.email_verified_at,
  failedLoginCount: r.failed_login_count,
  lockedUntil: r.locked_until,
  createdAt: r.created_at,
});

export async function findAccountByEmail(db: Db, email: string): Promise<Account | undefined> {
  const [rows] = await db.query<RowDataPacket[]>('SELECT * FROM accounts WHERE email = ?', [email]);
  return rows[0] && toAccount(rows[0]);
}

export async function findAccountById(db: Db, id: number): Promise<Account | undefined> {
  const [rows] = await db.query<RowDataPacket[]>('SELECT * FROM accounts WHERE id = ?', [id]);
  return rows[0] && toAccount(rows[0]);
}

export async function insertAccount(
  db: Db,
  a: Pick<Account, 'email' | 'passwordHash' | 'role' | 'emailVerifiedAt' | 'createdAt'>,
): Promise<number> {
  const [res] = await db.query<ResultSetHeader>(
    'INSERT INTO accounts (email, password_hash, role, email_verified_at, created_at) VALUES (?, ?, ?, ?, ?)',
    [a.email, a.passwordHash, a.role, a.emailVerifiedAt, a.createdAt],
  );
  return res.insertId;
}

/** Counts a failed login; once maxFailures is reached the account is locked and the counter restarts. */
export async function recordFailedLogin(db: Db, id: number, maxFailures: number, lockUntil: Date): Promise<void> {
  await db.query('UPDATE accounts SET failed_login_count = failed_login_count + 1 WHERE id = ?', [id]);
  await db.query(
    'UPDATE accounts SET locked_until = ?, failed_login_count = 0 WHERE id = ? AND failed_login_count >= ?',
    [lockUntil, id, maxFailures],
  );
}

export async function clearLoginFailures(db: Db, id: number): Promise<void> {
  await db.query('UPDATE accounts SET failed_login_count = 0, locked_until = NULL WHERE id = ?', [id]);
}

export async function markEmailVerified(db: Db, id: number, at: Date): Promise<void> {
  await db.query('UPDATE accounts SET email_verified_at = COALESCE(email_verified_at, ?) WHERE id = ?', [at, id]);
}

export async function setPasswordHash(db: Db, id: number, passwordHash: string): Promise<void> {
  await db.query('UPDATE accounts SET password_hash = ? WHERE id = ?', [passwordHash, id]);
}

export async function setRole(db: Db, id: number, role: Role): Promise<void> {
  await db.query('UPDATE accounts SET role = ? WHERE id = ?', [role, id]);
}
