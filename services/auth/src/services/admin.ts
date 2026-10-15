import type { Pool } from 'mysql2/promise';
import type { Clock } from '../clock.js';
import { withTransaction, type Db } from '../db/pool.js';
import { AppError } from '../errors.js';
import { findAccountById, setRole, type Account, type Role } from '../repositories/accounts.js';
import { insertBan, liftActiveBans } from '../repositories/bans.js';
import { revokeAllRefreshTokens } from '../repositories/refreshTokens.js';
import { toAccountView, type AccountView } from './auth.js';

export interface BanView {
  id: number;
  accountId: number;
  reason: string;
  expiresAt: string | null;
  bannedBy: number | null;
  createdAt: string;
}

async function requireAccount(db: Db, id: number): Promise<Account> {
  const account = await findAccountById(db, id);
  if (!account) throw new AppError(404, 'account_not_found', 'Account not found');
  return account;
}

/** Stops an admin from banning or demoting themselves and leaving no admin behind. */
function assertNotSelf(adminId: number, accountId: number): void {
  if (adminId === accountId) throw new AppError(400, 'cannot_modify_self', 'Admins cannot ban or change their own account');
}

export type AdminService = ReturnType<typeof createAdminService>;

export function createAdminService(deps: { pool: Pool; clock: Clock }) {
  const { pool, clock } = deps;

  return {
    /** Replaces any active ban and ends every session. Access JWTs already issued live until they expire. */
    async ban(adminId: number, accountId: number, input: { reason: string; expiresAt: Date | null }): Promise<BanView> {
      assertNotSelf(adminId, accountId);
      const now = clock.now();
      if (input.expiresAt && input.expiresAt <= now) {
        throw new AppError(400, 'validation_error', 'Request validation failed', [
          { path: 'expiresAt', message: 'expiresAt must be in the future' },
        ]);
      }
      const ban = await withTransaction(pool, async (conn) => {
        await requireAccount(conn, accountId);
        await liftActiveBans(conn, accountId, adminId, now);
        await revokeAllRefreshTokens(conn, accountId, now);
        return insertBan(conn, { accountId, reason: input.reason, expiresAt: input.expiresAt, bannedBy: adminId, createdAt: now });
      });
      return {
        ...ban,
        expiresAt: ban.expiresAt?.toISOString() ?? null,
        createdAt: ban.createdAt.toISOString(),
      };
    },

    async unban(adminId: number, accountId: number): Promise<void> {
      await requireAccount(pool, accountId);
      await liftActiveBans(pool, accountId, adminId, clock.now());
    },

    /** Takes effect in the next access token the account is issued. */
    async setRole(adminId: number, accountId: number, role: Role): Promise<AccountView> {
      assertNotSelf(adminId, accountId);
      const account = await requireAccount(pool, accountId);
      await setRole(pool, accountId, role);
      return toAccountView({ ...account, role });
    },
  };
}
