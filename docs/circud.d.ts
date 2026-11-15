/** Complete public TypeScript contract for the native `circud` ES module. */
declare module "circud" {

export type JsonPrimitive = null | boolean | number | string;

export interface JsonObject {
    readonly [key: string]: JsonValue;
}

export type JsonValue =
    | JsonPrimitive
    | JsonObject
    | readonly JsonValue[];

export type ByteSource = ArrayBuffer | ArrayBufferView;

export interface CircudAbortEvent {
    readonly type: "abort";
    readonly target: AbortSignal;
    readonly currentTarget: AbortSignal;
}

export type CircudAbortListener = (
    this: AbortSignal,
    event: CircudAbortEvent,
) => unknown;

export interface CircudAbortListenerObject {
    handleEvent(event: CircudAbortEvent): unknown;
}

export type CircudAbortEventListener =
    | CircudAbortListener
    | CircudAbortListenerObject;

/**
 * Runtime-owned cancellation signal. Packages receive instances from call and
 * endpoint events; they cannot construct signals themselves. Abortion fires
 * once and listeners run on the owning package runtime thread.
 */
export class AbortSignal {
    private constructor();
    readonly aborted: boolean;
    readonly reason: unknown;
    onabort: CircudAbortListener | null;
    addEventListener(
        type: "abort",
        listener: CircudAbortEventListener | null,
        options?: boolean | { capture?: boolean; once?: boolean },
    ): void;
    removeEventListener(
        type: "abort",
        listener: CircudAbortEventListener | null,
        options?: boolean | { capture?: boolean },
    ): void;
    throwIfAborted(): void;
}

export type EndpointHeaders = Readonly<Record<string, string>>;

export interface EndpointResponse {
    /** Finite integer from 200 through 599. Defaults to 200. */
    status?: number;
    headers?: EndpointHeaders;
    /** Objects and arrays are JSON encoded; null and undefined produce no body. */
    body?: JsonValue | ByteSource | undefined;
}

export interface SseAcceptOptions {
    /** Defaults to 200. */
    status?: 200;
    headers?: EndpointHeaders;
}

export interface WebSocketAcceptOptions {
    /** The WebSocket handshake status. The normal value is 101. */
    status?: 101;
    headers?: EndpointHeaders;
    /** Must be one of EndpointEvent.offeredSubprotocols. */
    subprotocol?: string;
}

export interface SseSession {
    readonly id: number;
    /** Send a non-empty, already framed UTF-8 SSE record, including its final blank line. */
    send(text: string): boolean;
    close(): boolean;
    onclose?:
        | ((this: SseSession, event: EndpointCloseEvent) => unknown)
        | null;
}

export interface WebSocketSession {
    readonly id: number;
    send(value: string | ByteSource): boolean;
    /**
     * Use a valid RFC 6455 wire code: 1000-1003, 1007-1013, or 3000-4999.
     * Reserved codes such as 1005 and 1006 are rejected.
     */
    close(code?: number, reason?: string): boolean;
    onmessage?:
        | ((this: WebSocketSession, event: EndpointMessageEvent) => unknown)
        | null;
    onclose?:
        | ((this: WebSocketSession, event: EndpointCloseEvent) => unknown)
        | null;
}

export type EndpointSession = SseSession | WebSocketSession;

export interface EndpointEvent {
    readonly id: number;
    /** SSE requests begin with the "http" protocol. */
    readonly protocol: "http" | "websocket";
    /** The original valid HTTP method token, including extension methods. */
    readonly method: string;
    /** Path relative to the package's manifest prefix. */
    readonly path: string;
    /** Query text without the leading question mark. */
    readonly query: string;
    readonly headers: readonly (readonly [name: string, value: string])[];
    readonly body: Uint8Array;
    readonly offeredSubprotocols: readonly string[];
    readonly remoteAddress: string;
    /**
     * Advisory authentication state. circud does not enforce a package's
     * public/401/403 policy; the package must make that decision itself.
     */
    readonly isAuthorized: boolean;
    readonly signal: AbortSignal;

    /**
     * Select an ordinary response during synchronous EvBus dispatch. The
     * supplied native Promise may settle later; arbitrary thenables are not
     * accepted.
     */
    respondWith(
        response: EndpointResponse | Promise<EndpointResponse>,
    ): void;

    /** Select one installed-package file during synchronous EvBus dispatch. */
    serveStatic(relativePath: string): void;

    /** Select an SSE response during synchronous EvBus dispatch. */
    acceptSse(options?: SseAcceptOptions): SseSession | null;

    /** Select a WebSocket upgrade during synchronous EvBus dispatch. */
    acceptWebSocket(
        options?: WebSocketAcceptOptions,
    ): WebSocketSession | null;
}

export type EndpointMessageEvent =
    | {
        readonly id: number;
        readonly session: WebSocketSession;
        readonly data: string;
        readonly binary: false;
    }
    | {
        readonly id: number;
        readonly session: WebSocketSession;
        readonly data: Uint8Array;
        readonly binary: true;
    };

export interface EndpointCloseEvent {
    readonly id: number;
    /** Absent when an ordinary response Promise is cancelled before accept. */
    readonly session: EndpointSession | undefined;
    /** 1005 means the peer sent a Close frame without a status code. */
    readonly code: number;
    readonly reason: string;
}

export interface ManagementCallEvent<Arguments = unknown, Result = unknown> {
    readonly id: number;
    /** "core", "web", or the calling package ID. */
    readonly source: string;
    readonly target: string;
    readonly arguments: Arguments;
    readonly signal: AbortSignal;
    /**
     * Must be called synchronously during dispatch and at most once. A native
     * Promise may settle later; arbitrary thenables are not accepted.
     */
    respondWith(value: Result | Promise<Result>): void;
}

export interface HostCallOptions {
    /** Positive milliseconds, capped by circud at five minutes. */
    timeoutMs?: number;
    /** ArrayBuffers whose ownership is transferred into the target runtime. */
    transfer?: readonly ArrayBuffer[];
}

export type PackageCallErrorCode =
    | "not_found"
    | "unavailable"
    | "package_calls_disabled"
    | "unhandled_call"
    | "overloaded"
    | "timeout"
    | "cancelled"
    | "remote_error";

export interface PackageCallError extends Error {
    code: PackageCallErrorCode;
    remoteName?: string;
    runtimeState?: string;
}

export interface HostInfo {
    readonly packageId: string;
    readonly packageDir: string;
    readonly dataDir: string;
}

export function call<Result = unknown, Arguments = unknown>(
    packageId: string,
    target: string,
    arguments: Arguments,
    options?: HostCallOptions,
): Promise<Result>;

export function reload(): void;

export function info(): HostInfo | null;

export interface ApiError {
    code: string;
    message: string;
}

export interface ApiSuccess<Result> {
    ok: true;
    result: Result;
}

export interface ApiFailure {
    ok: false;
    error: ApiError;
}

export type ApiEnvelope<Result> = ApiSuccess<Result> | ApiFailure;

export interface AuthenticationDisabledSession {
    authEnabled: false;
    authenticated: false;
}

export interface AnonymousSession {
    authEnabled: true;
    authenticated: false;
}

export interface AuthenticatedSession {
    authEnabled: true;
    authenticated: true;
    username: string;
    /** Send this value in X-Circud-CSRF on state-changing management calls. */
    csrfToken: string;
    /** Unix timestamp in milliseconds. */
    expiresAt: number;
}

export type Session =
    | AuthenticationDisabledSession
    | AnonymousSession
    | AuthenticatedSession;

export interface LoginRequest {
    username: string;
    password: string;
}

export type SessionResponse = ApiEnvelope<Session>;
export type LoginResponse = ApiEnvelope<AuthenticatedSession>;
/** POST /api/auth/logout succeeds with HTTP 204 and no response body. */
export type LogoutResponse = undefined;

export interface PackagePermissions {
    read: string[];
    write: string[];
    env: string[];
    net: boolean;
    run: boolean;
    ffi: boolean;
    sys: boolean;
}

export interface PackageManifest {
    schema: 1;
    id: string;
    name: string;
    version: string;
    entry: string;
    acceptPackageCalls: boolean;
    shutdownTimeoutMs: number;
    /** Omitted when the effective prefix is the default /<package-id>. */
    prefix?: string;
    permissions: PackagePermissions;
}

export type RuntimeState =
    | "disabled"
    | "starting"
    | "running"
    | "stopping"
    | "failed"
    | "stuck";

export type PackageStatusState =
    | "starting"
    | "running"
    | "stopped"
    | "degraded"
    | "failed"
    | "unknown";

export interface PackageStatusValue {
    state: PackageStatusState;
    message: string;
}

/** Value returned by a package's own `status.get` management handler. */
export interface PackageStatusResponse {
    state: PackageStatusState;
    message?: string;
}

export interface LifecycleStopArguments {
    reason: string;
}

export interface StatusSnapshot {
    value: PackageStatusValue | null;
    stale: boolean;
    error: string;
    consecutiveTimeouts: number;
    /** Unix timestamp in milliseconds; zero means no collection yet. */
    collectedAtMs: number;
    /** Unix timestamp in milliseconds; zero means no successful collection. */
    lastSuccessAtMs: number;
}

export interface InstalledPackage extends PackageManifest {
    enabled: boolean;
    runtimeState: RuntimeState;
    runtimeError?: string;
}

export interface ListedPackage extends InstalledPackage {
    status: StatusSnapshot | null;
}

export interface SystemInfo {
    version: string;
    apiVersion: 1;
    limits: {
        rpcBodyBytes: number;
        uploadBytes: number;
        staticFileBytes: number;
        defaultCallTimeoutMs: number;
        maxCallTimeoutMs: number;
    };
    statusPolling: {
        enabled: boolean;
        intervalMs: number;
        timeoutMs: number;
    };
    events: {
        enabled: boolean;
        capacity: number;
        maxQueryLimit: number;
        persistent: false;
    };
}

export type ManagementEventLevel = "info" | "warning" | "error";

export interface ManagementEvent {
    id: number;
    timestampMs: number;
    level: ManagementEventLevel;
    category: string;
    package: string | null;
    action: string;
    code: string;
    message: string;
    details: JsonObject;
}

export interface EventsListParams {
    afterId?: number;
    /** Integer in the inclusive range 1..200. */
    limit?: number;
    level?: ManagementEventLevel;
    category?: string;
    package?: string;
}

export interface EventsPage {
    events: ManagementEvent[];
    journalId: string;
    oldestId: number;
    latestId: number;
    nextAfterId: number;
    droppedBeforeId: number;
    gap: boolean;
    reset: boolean;
    hasMore: boolean;
}

export interface PackageIdParams {
    id: string;
}

export interface PackageRemoveParams extends PackageIdParams {
    purgeData?: boolean;
}

export interface StatusGetParams extends PackageIdParams {
    refresh?: boolean;
}

export interface HttpPackageCallParams {
    package: string;
    target: string;
    arguments?: JsonValue;
    /** Positive milliseconds, no more than five minutes. */
    timeoutMs?: number;
}

export interface PackageCallFailure {
    ok: false;
    error: {
        code: PackageCallErrorCode;
        message: string;
        remoteName: string;
        runtimeState: string;
    };
}

export interface PackageCallSuccess {
    ok: true;
    value: JsonValue;
}

export type HttpPackageCallResult = PackageCallSuccess | PackageCallFailure;

export interface RpcParamsMap {
    "system.info": Record<string, never>;
    "events.list": EventsListParams;
    "packages.list": Record<string, never>;
    "package.remove": PackageRemoveParams;
    "package.start": PackageIdParams;
    "package.stop": PackageIdParams;
    "package.reload": PackageIdParams;
    "status.get": StatusGetParams;
    call: HttpPackageCallParams;
}

export interface RpcResultMap {
    "system.info": SystemInfo;
    "events.list": EventsPage;
    "packages.list": ListedPackage[];
    "package.remove": { ok: true };
    "package.start": { ok: true; runtimeState: RuntimeState };
    "package.stop": { ok: true; runtimeState: RuntimeState };
    "package.reload": { ok: true; runtimeState: RuntimeState };
    "status.get": StatusSnapshot | null;
    call: HttpPackageCallResult;
}

export type RpcMethod = keyof RpcParamsMap;
export type RpcMethodWithoutRequiredParams =
    | "system.info"
    | "events.list"
    | "packages.list";

export type RpcRequest<Method extends RpcMethod = RpcMethod> =
    Method extends RpcMethodWithoutRequiredParams
    ? { method: Method; params?: RpcParamsMap[Method] }
    : { method: Method; params: RpcParamsMap[Method] };

export type RpcResult<Method extends RpcMethod> = RpcResultMap[Method];
export type RpcResponse<Method extends RpcMethod> = ApiEnvelope<
    RpcResult<Method>
>;

export type PackageInstallResponse = ApiEnvelope<InstalledPackage>;
export interface HealthResponse {
    ok: true;
}

/** Fixed HTTP routes and header names in the management API. */
export interface CircudHttpEndpoints {
    readonly health: "/health";
    readonly rpc: "/rpc";
    readonly session: "/api/auth/session";
    readonly login: "/api/auth/login";
    readonly logout: "/api/auth/logout";
    readonly packageInstall: "/api/packages/install";
    readonly csrfHeader: "X-Circud-CSRF";
}

}

