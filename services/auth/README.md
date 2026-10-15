# Kast Authentication Server

Account, session and realm-list API for Kast MMORPGs. TypeScript (strict, ESM), Node 24, Express 5, MySQL 8.4.

- Email + password accounts with email verification
- RS256 access tokens (15 min) published via JWKS, rotating refresh tokens (30 days) with reuse detection
- Password reset, per-IP rate limits and account lockout (state stored in MySQL)
- Bans, roles (`player`, `gm`, `admin`), realm list, admin endpoints

Full API contract: [`openapi.yaml`](openapi.yaml) (OpenAPI 3.1).

## Setup

```sh
cd services/auth
cp .env.example .env        # dev defaults; change the passwords
docker compose up -d --build
curl -s localhost:3000/health   # {"status":"ok","db":"ok"}
```

| Service | Port | Notes |
|---|---|---|
| `auth` | 3000 | Runs migrations, creates or loads the signing key and seeds the admin on start |
| `mysql` | `${MYSQL_PORT:-3306}` | MySQL 8.4, volume `mysql-data`. First start also creates `kast_auth_test` |
| `mailpit` | 8025 (UI/API), 1025 (SMTP) | Every email the service sends shows up at http://localhost:8025 |

`docker compose down` stops the stack and keeps the data. `docker compose down -v` also deletes the database and the dev signing key.

### Running on the host

To run the service on the host instead, keep `mysql` and `mailpit` in compose:

```sh
npm ci
docker compose up -d mysql mailpit
npm run dev          # tsx, reads .env
```

## Commands

| Command | What it does |
|---|---|
| `npm run build` | Compile `src/` to `dist/` |
| `npm run typecheck` | `tsc --noEmit` over src and tests |
| `npm run lint` | ESLint (typescript-eslint recommended) |
| `npm test` | Vitest + supertest against the compose MySQL, database `TEST_DB_NAME` |
| `npm run migrate` | Apply pending migrations to `DB_NAME`. In the container: `docker compose exec auth node dist/migrate-cli.js` |
| `npm run dev` | Run the server from source |

`npm test` needs `.env` and the compose `mysql` running. One test also needs `mailpit`: it sends a real email over SMTP and reads it back through the Mailpit API (`MAILPIT_API_URL`, default `http://127.0.0.1:8025`). The other tests use an in-memory mailer and a fake clock, so the lockout and expiry tests don't sleep. Test files run one at a time and empty the test database before each test.

### Migrations

Migrations are plain `migrations/NNN_name.sql` files. They run in filename order, once each, and are tracked in `schema_migrations`. The server applies pending migrations on every start, and a MySQL named lock keeps two instances from migrating at the same time. Never edit a migration that has already been applied; add a new one. MySQL DDL auto-commits, so if a file fails partway through you have to repair it by hand.

## Configuration

All configuration comes from environment variables. They are validated with zod at startup, and the process exits with a list of every bad or missing variable. docker compose reads `.env` and then overrides `DB_HOST`, `DB_PORT`, `SMTP_HOST`, `SMTP_PORT`, `PORT` and `KEYS_DIR` for the container network.

| Variable | Default | Description |
|---|---|---|
| `PORT` | `3000` | HTTP port |
| `TRUST_PROXY` | `false` | Express `trust proxy` (`true`, a hop count, or subnets). Set this behind a load balancer so rate limits see the client IP |
| `APP_BASE_URL` | required | Base URL of the page that handles email links: `<base>/verify-email?token=…` and `<base>/reset-password?token=…`. That page POSTs the token to this API |
| `DB_HOST` / `DB_PORT` | required / `3306` | MySQL address |
| `DB_USER` / `DB_PASSWORD` / `DB_NAME` | required | MySQL credentials and database. compose also uses them to create the user and database |
| `TEST_DB_NAME` | `kast_auth_test` | Database used by `npm test` |
| `MYSQL_ROOT_PASSWORD` | required by compose | Root password of the mysql container |
| `MYSQL_PORT` | `3306` | Host port published by the mysql container |
| `SMTP_HOST` / `SMTP_PORT` | required | SMTP server (Mailpit: `127.0.0.1:1025`) |
| `SMTP_SECURE` | `false` | `true` for implicit TLS (port 465) |
| `SMTP_USER` / `SMTP_PASSWORD` | empty | SMTP auth. Leave empty for none |
| `MAIL_FROM` | required | From header |
| `JWT_ISSUER` / `JWT_AUDIENCE` | required | `iss` / `aud` claims |
| `ACCESS_TOKEN_TTL_SECONDS` | `900` | Access token lifetime |
| `REFRESH_TOKEN_TTL_DAYS` | `30` | Refresh token lifetime. Each rotation starts a new full period |
| `EMAIL_VERIFY_TTL_HOURS` | `24` | Verification link lifetime |
| `PASSWORD_RESET_TTL_MINUTES` | `60` | Reset link lifetime |
| `JWT_PRIVATE_KEY` | empty | RSA private key, PKCS#8 or PKCS#1 PEM. `\n` escapes are accepted |
| `JWT_PRIVATE_KEY_FILE` | empty | Path to the PEM file instead, e.g. a mounted secret. Set only one of the two |
| `JWT_KID` | RFC 7638 thumbprint | Key id put in the JWT header and the JWKS |
| `KEYS_DIR` | `./keys` | Where the dev key is generated when no key is configured. compose uses `/app/keys` on volume `auth-keys` |
| `SEED_ADMIN_EMAIL` / `SEED_ADMIN_PASSWORD` | empty | First admin, created pre-verified on startup if no account has that email. Set both or neither. An existing account with that email is never changed |

Production: always set `JWT_PRIVATE_KEY` or `JWT_PRIVATE_KEY_FILE`. The dev-key fallback is meant for local use only. To generate a key: `openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:2048 -out jwt-private.pem`.

## Endpoints

| Method | Path | Auth | Description |
|---|---|---|---|
| GET | `/health` | none | Liveness + `SELECT 1`. 200 or 503 `db_unavailable` |
| GET | `/.well-known/jwks.json` | none | Public signing keys |
| POST | `/v1/auth/register` | none | `{email,password}` → 202. Same response for taken emails. 5/h/IP |
| POST | `/v1/auth/verify-email` | none | `{token}` → 200. Single use, 24h |
| POST | `/v1/auth/resend-verification` | none | `{email}` → 202 always. 5/h/IP |
| POST | `/v1/auth/login` | none | `{email,password}` → token pair. 20/min/IP |
| POST | `/v1/auth/refresh` | none | `{refreshToken}` → new pair. The old token is consumed |
| POST | `/v1/auth/logout` | none | `{refreshToken}` → 204. Revokes that session |
| POST | `/v1/auth/forgot-password` | none | `{email}` → 202 always. 5/h/IP |
| POST | `/v1/auth/reset-password` | none | `{token,password}` → 200. Revokes all sessions |
| GET | `/v1/me` | bearer | id, email, role, emailVerified, createdAt |
| GET | `/v1/realms` | bearer | Realms ordered by `sortOrder` |
| POST | `/v1/admin/accounts/:id/ban` | admin | `{reason, expiresAt?}` → 201. Replaces the active ban and revokes sessions |
| DELETE | `/v1/admin/accounts/:id/ban` | admin | Lift the ban → 204 |
| PUT | `/v1/admin/accounts/:id/role` | admin | `{role}`: `player`, `gm` or `admin` |
| GET, POST | `/v1/admin/realms` | admin | List / create |
| GET, PUT, DELETE | `/v1/admin/realms/:id` | admin | Read / replace / delete |

### Errors

Every error has this shape: `{"error":{"code":"snake_case","message":"…","details":…}}`. `details` is present only for `validation_error` (a list of `{path,message}`), `account_banned` (`{reason,expiresAt}`) and `account_locked` (`{lockedUntil}`). Responses never include stack traces or SQL.

| Status | Codes |
|---|---|
| 400 | `validation_error`, `invalid_token`, `cannot_modify_self` |
| 401 | `unauthorized`, `invalid_credentials`, `invalid_refresh_token` |
| 403 | `email_not_verified`, `account_banned`, `forbidden` |
| 404 | `not_found`, `account_not_found`, `realm_not_found` |
| 409 | `realm_name_taken` |
| 413 | `payload_too_large` (JSON body limit is 10 kB) |
| 423 | `account_locked` |
| 429 | `rate_limited`, with a `Retry-After` header |
| 500 / 503 | `internal_error` / `db_unavailable` |

## Security behaviour

- **No account discovery.**
  - register, resend-verification and forgot-password return identical responses for every email.
  - Login returns `invalid_credentials` for both an unknown email and a wrong password. An unknown email still runs a bcrypt comparison, so both cases take about the same time.
  - Emails are sent fire-and-forget, so the time to send an email doesn't show up in response times either.
- **Login check order.** Checks run in this order: lockout, password, email verified, ban. `email_not_verified` and `account_banned` are only returned once the password is correct.
- **Lockout.**
  - 5 consecutive wrong passwords lock the account for 15 minutes. While it's locked, even the correct password gets `423 account_locked`.
  - A successful login or a password reset clears the counter.
  - Trade-off: lockout only happens to accounts that exist, so a client that sends 5 wrong passwords can learn whether that email is registered.
- **Rate limiting.**
  - Fixed windows per IP, stored in MySQL, so limits survive restarts and are shared by every instance.
  - Expired rows are deleted on each write.
  - Because the windows are fixed, a client can send up to twice the limit in a short burst across a window boundary.
- **Passwords.**
  - Passwords must be 10–128 characters and are hashed with bcrypt at cost 12.
  - bcrypt only reads the first 72 bytes, so each password is first SHA-256 hashed (base64). That way every character of a 128-character password counts.
- **Tokens at rest.** Refresh, verification and reset tokens are 256-bit random values. Only their SHA-256 hashes are stored.
- **Refresh rotation.**
  - Each refresh consumes the token it was given.
  - Presenting an already-rotated token revokes the whole session (token family).
  - The check is strict: if a client sends two refreshes in parallel with the same token, its session is revoked.
- **Logs** never contain passwords, tokens, request bodies or SQL parameters.

## Verifying access tokens from the World Server

The World Server only needs the public keys:

1. Fetch `GET {auth}/.well-known/jwks.json` and cache it (it is served with `max-age=300`). When a token carries a `kid` you don't have cached, refetch once, which handles key rotation.
2. Verify each access token:
   - `alg` must be `RS256`. Reject anything else, including `none` and HS*.
   - Choose the key by the `kid` header.
   - `iss` must equal `JWT_ISSUER` and `aud` must equal `JWT_AUDIENCE`.
   - `exp` must be in the future. Allow at most a few seconds of clock skew.
3. Use the claims: `sub` is the account id (a string), plus `email` and `role` (`player`, `gm` or `admin`).

Node example (`jose`):

```ts
import { createRemoteJWKSet, jwtVerify } from 'jose';
const jwks = createRemoteJWKSet(new URL('http://auth:3000/.well-known/jwks.json'));
const { payload } = await jwtVerify(token, jwks, { issuer: 'kast-auth', audience: 'kast', algorithms: ['RS256'] });
```

**Ban and role gap (up to 15 minutes).**
- Banning an account revokes its refresh tokens immediately, so it can't get new access tokens.
- An access token that was already issued keeps working until its `exp`, which is at most `ACCESS_TOKEN_TTL_SECONDS` (15 min).
- A role change also only takes effect in the next token.
- If the World Server needs to kick a banned player sooner, it has to ask this service. That would need a new endpoint; none exists today.

### Key rotation

The JWKS currently publishes a single key. To rotate:
1. Deploy the new key.
2. Tokens signed by the old key fail verification once the new JWKS is fetched, so players have to refresh. Their refresh tokens are unaffected.

A zero-downtime rotation would need the JWKS to publish both keys during an overlap window. That isn't built yet.
