/**
 * Browser-facing subset used by the static circud management UI.
 * The complete authoritative public type declarations live in
 * ../docs/circud.d.ts; keep this file limited to browser HTTP calls.
 */

export interface ApiError {
  code: string;
  message: string;
}

export interface ApiSuccess<T> {
  ok: true;
  result: T;
}

export interface ApiFailure {
  ok: false;
  error: ApiError;
}

export type ApiEnvelope<T> = ApiSuccess<T> | ApiFailure;

export interface AuthenticationDisabledSession {
  authEnabled: false;
  authenticated: false;
}

export interface AnonymousSession {
  authEnabled: true;
  authenticated: false;
}

export interface AuthSession {
  authEnabled: true;
  authenticated: true;
  username: string;
  /** Sent in X-Circud-CSRF for every protected management request. */
  csrfToken: string;
  /** Server-provided session expiry timestamp. */
  expiresAt: number;
}

export type Session =
  | AuthenticationDisabledSession
  | AnonymousSession
  | AuthSession;

export interface LoginRequest {
  username: string;
  password: string;
}

export interface RpcRequest<P extends object = Record<string, unknown>> {
  method: string;
  params: P;
}

export type SessionResponse = ApiEnvelope<Session>;
export type LoginResponse = ApiEnvelope<AuthSession>;
export type RpcResponse<T> = ApiEnvelope<T>;
export type PackageInstallResponse<T = unknown> = ApiEnvelope<T>;

/**
 * GET /api/auth/session
 *   Public. Returns SessionResponse and never requires a CSRF header. When
 *   authEnabled is false, protected management requests also omit CSRF.
 *
 * POST /api/auth/login
 *   Public JSON LoginRequest. Returns LoginResponse and sets the HttpOnly
 *   session cookie on success.
 *
 * POST /api/auth/logout
 *   With authentication enabled, requires the session cookie and
 *   X-Circud-CSRF. Returns 204 with no body.
 *
 * POST /rpc
 *   With authentication enabled, requires the session cookie and
 *   X-Circud-CSRF. Accepts RpcRequest and returns RpcResponse<T>.
 *
 * POST /api/packages/install
 *   With authentication enabled, requires the session cookie and
 *   X-Circud-CSRF. Body is application/zip; returns PackageInstallResponse.
 */
export interface CircudBrowserEndpoints {
  readonly session: '/api/auth/session';
  readonly login: '/api/auth/login';
  readonly logout: '/api/auth/logout';
  readonly rpc: '/rpc';
  readonly packageInstall: '/api/packages/install';
  readonly csrfHeader: 'X-Circud-CSRF';
}
