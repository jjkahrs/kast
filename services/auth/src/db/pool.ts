import mysql, { type Connection, type Pool, type PoolOptions } from 'mysql2/promise';
import type { Config } from '../config.js';

/** Anything that can run a query: the pool, or a connection inside a transaction. */
export type Db = Connection;

export function connectionOptions(config: Config): PoolOptions {
  return {
    host: config.DB_HOST,
    port: config.DB_PORT,
    user: config.DB_USER,
    password: config.DB_PASSWORD,
    database: config.DB_NAME,
    // DATETIME columns hold UTC; read and write JS Dates as UTC.
    timezone: 'Z',
  };
}

export function createPool(config: Config): Pool {
  return mysql.createPool({ ...connectionOptions(config), connectionLimit: 10 });
}

export async function withTransaction<T>(pool: Pool, work: (conn: Db) => Promise<T>): Promise<T> {
  const conn = await pool.getConnection();
  try {
    await conn.beginTransaction();
    const result = await work(conn);
    await conn.commit();
    return result;
  } catch (err) {
    await conn.rollback();
    throw err;
  } finally {
    conn.release();
  }
}

export function isDuplicateKey(err: unknown): boolean {
  return (err as { code?: string }).code === 'ER_DUP_ENTRY';
}
