import { decodeJwt } from 'jose';
import { describe, expect, it } from 'vitest';
import { findAccountByEmail } from '../src/repositories/accounts.js';
import { runUserCommand } from '../src/user-cli.js';
import { createAccount, login, PASSWORD, useTestApp } from './helpers.js';

describe('user CLI', () => {
  const ctx = useTestApp();
  const run = (argv: string[], prompted = PASSWORD) =>
    runUserCommand(ctx.pool, argv, async () => prompted, ctx.clock);

  it('creates a verified account with the role that can log in', async () => {
    const out = await run(['create', ' New@Kast.Local ', 'gm', '--password', PASSWORD]);
    expect(out).toMatch(/^Created gm account new@kast\.local \(id \d+\)$/);
    const account = await findAccountByEmail(ctx.pool, 'new@kast.local');
    expect(account?.role).toBe('gm');
    expect(account?.emailVerifiedAt).not.toBeNull();
    const session = await login(ctx, 'new@kast.local');
    expect(decodeJwt(session.accessToken).role).toBe('gm');
  });

  it('prompts for the password when --password is omitted', async () => {
    await run(['create', 'prompted@kast.local', 'admin'], 'prompted password');
    await login(ctx, 'prompted@kast.local', 'prompted password');
  });

  it('refuses a duplicate email', async () => {
    await createAccount(ctx, 'dup@kast.local');
    await expect(run(['create', 'dup@kast.local', 'admin'])).rejects.toThrow(/already exists; use set-role/);
    expect((await findAccountByEmail(ctx.pool, 'dup@kast.local'))?.role).toBe('player');
  });

  it('rejects bad input without touching the database', async () => {
    await expect(run(['create', 'not-an-email', 'gm'])).rejects.toThrow(/email/);
    await expect(run(['create', 'a@kast.local', 'god'])).rejects.toThrow(/role/);
    await expect(run(['create', 'a@kast.local', 'gm'], 'short')).rejects.toThrow(/password/);
    await expect(run(['create', 'a@kast.local'])).rejects.toThrow(/Expected a command/);
    await expect(run(['delete', 'a@kast.local', 'gm'])).rejects.toThrow(/Unknown command/);
    await expect(run(['create', 'a@kast.local', 'gm', '--bogus'])).rejects.toThrow(/bogus/);
    expect(await findAccountByEmail(ctx.pool, 'a@kast.local')).toBeUndefined();
  });

  it('set-role changes the role of an existing account', async () => {
    const id = await createAccount(ctx, 'promote@kast.local');
    expect(await run(['set-role', 'Promote@kast.local', 'admin'])).toBe(
      `Changed promote@kast.local (id ${id}) from player to admin`,
    );
    expect((await findAccountByEmail(ctx.pool, 'promote@kast.local'))?.role).toBe('admin');
  });

  it('set-role fails for an unknown email', async () => {
    await expect(run(['set-role', 'ghost@kast.local', 'gm'])).rejects.toThrow(/No account with email/);
  });
});
