// Authenticated same-origin proxy for circud's bounded management surface.
// Package endpoints intentionally remain available only on the native listener.
'use strict';

import * as socket from 'socket';
import { readfile } from 'fs';

const MAX_REQUEST_BYTES = 64 * 1024 * 1024;
const MAX_RESPONSE_BYTES = 64 * 1024 * 1024;
const IO_TIMEOUT_MS = 30000;
const CONFIG_PATH = '/etc/circud/config.json';
const PROXY_COOKIE = 'luci_circud_session';
const UPSTREAM_COOKIE = 'circud_session';
const MAX_COOKIE_BYTES = 8 * 1024;
const MAX_COOKIE_PAIRS = 128;
const MAX_COOKIE_AGE = 10 * 365 * 24 * 60 * 60;
const HOP_HEADERS = [
	'connection', 'keep-alive', 'proxy-authenticate', 'proxy-authorization',
	'te', 'trailer', 'transfer-encoding', 'upgrade'
];

let http;

function valid_token(value) {
	return type(value) == 'string' &&
		length(value) == 43 &&
		!!match(value, /^[A-Za-z0-9_-]{43}$/);
}

function proxy_cookie_path(env) {
	let script = http.getenv('SCRIPT_NAME') || '';
	let matched_script = match(script, /^\/[A-Za-z0-9._~\/-]*$/);
	if (!matched_script || matched_script[0] != script)
		return null;
	let path = env.ctx?.path;
	if (type(path) != 'array' || !length(path))
		return null;
	for (let segment in path) {
		let matched_segment = type(segment) == 'string'
			? match(segment, /^[A-Za-z0-9_.~-]+$/) : null;
		if (!matched_segment || matched_segment[0] != segment)
			return null;
	}
	return script + '/' + join('/', path) + '/';
}

function parse_session_cookie(source) {
	if (!source)
		return { token: null };
	if (length(source) > MAX_COOKIE_BYTES)
		return { invalid: true };

	let token = null, pairs = split(source, ';');
	if (length(pairs) > MAX_COOKIE_PAIRS)
		return { invalid: true };
	for (let pair in pairs) {
		pair = trim(pair);
		let equals = index(pair, '=');
		if (equals < 0)
			continue;
		let name = trim(substr(pair, 0, equals));
		if (name != PROXY_COOKIE)
			continue;
		let value = trim(substr(pair, equals + 1));
		if (token != null || !valid_token(value))
			return { invalid: true };
		token = value;
	}
	return { token };
}

function request_session_cookie() {
	return parse_session_cookie(http.getenv('HTTP_COOKIE'));
}

function upstream_session_cookie(value) {
	if (type(value) != 'string' || length(value) > MAX_COOKIE_BYTES)
		return null;

	let parts = split(value, ';');
	let first = trim(parts[0]);
	let equals = index(first, '=');
	if (equals < 0 || trim(substr(first, 0, equals)) != UPSTREAM_COOKIE)
		return null;
	let token = trim(substr(first, equals + 1));
	if (length(token) && !valid_token(token))
		return null;

	let attributes = {}, secure = false, max_age = null;
	for (let i = 1; i < length(parts); i++) {
		let part = parts[i];
		part = trim(part);
		let split_at = index(part, '=');
		let name = lc(trim(split_at < 0 ? part : substr(part, 0, split_at)));
		let attribute = split_at < 0 ? null :
			trim(substr(part, split_at + 1));
		if (!length(name) || attributes[name] === true)
			return null;
		attributes[name] = true;

		switch (name) {
		case 'path':
			if (attribute != '/') return null;
			break;
		case 'httponly':
			if (attribute != null) return null;
			break;
		case 'samesite':
			if (lc(attribute || '') != 'strict') return null;
			break;
		case 'secure':
			if (attribute != null) return null;
			secure = true;
			break;
		case 'max-age':
			if (!match(attribute || '', /^[0-9]+$/)) return null;
			max_age = +attribute;
			break;
		default:
			return null;
		}
	}

	if (!attributes.path || !attributes.httponly || !attributes.samesite ||
		!attributes['max-age'] || max_age < 0 || max_age > MAX_COOKIE_AGE ||
		(length(token) ? max_age == 0 : max_age != 0))
		return null;

	return { token, max_age, secure };
}

function rewrite_session_cookie(cookie, path, secure_transport) {
	let value = PROXY_COOKIE + '=' + cookie.token + '; Path=' + path +
		'; HttpOnly; SameSite=Strict';
	if (cookie.secure || secure_transport)
		value += '; Secure';
	return value + '; Max-Age=' + cookie.max_age;
}

function send_all(sock, data) {
	let offset = 0;
	while (offset < length(data)) {
		let written = sock.send(substr(data, offset));
		if (written == null || written <= 0)
			return false;
		offset += written;
	}

	return true;
}

function read_response(sock) {
	let chunks = [], total = 0;
	while (true) {
		let ready = socket.poll(IO_TIMEOUT_MS, sock);
		let events = ready?.[0]?.[1] || 0;
		if (!(events & (socket.POLLIN | socket.POLLHUP)))
			return null;
		let part = sock.recv(16384);
		if (part == null)
			return null;
		if (length(part) == 0)
			break;
		total += length(part);
		if (total > MAX_RESPONSE_BYTES)
			return null;
		push(chunks, part);
	}

	return join('', chunks);
}

function target() {
	let config = {};
	try {
		config = json(readfile(CONFIG_PATH) || '{}');
	}
	catch (error) {
		return null;
	}

	if (config?.web?.enabled === false)
		return null;
	let address = config?.listen?.address || '127.0.0.1';
	let configured_port = config?.listen?.port;
	let port = configured_port == null ? 9080 : +configured_port;
	if (type(address) != 'string' ||
		(configured_port != null && type(configured_port) != 'int') ||
		port < 1 || port > 65535)
		return null;
	if (address == '0.0.0.0') address = '127.0.0.1';
	if (address == '::') address = '::1';

	return { address, port };
}

function is_management_path(path) {
	if (index([
		'/', '/index.html', '/app.js', '/app.css',
		'/circud-api.d.ts', '/favicon.ico', '/health', '/rpc',
		'/api/auth/session', '/api/auth/login', '/api/auth/logout',
		'/api/packages/install'
	], path) >= 0)
		return true;

	return path == '/assets' || substr(path, 0, 8) == '/assets/';
}

function find_header_end(raw) {
	let end = index(raw, '\r\n\r\n');
	return end >= 0 ? [ end, 4 ] : [ index(raw, '\n\n'), 2 ];
}

function proxy_response(raw, method, cookie_path) {
	let boundary = find_header_end(raw);
	if (boundary[0] < 0) {
		http.status(502, 'Bad Gateway');
		http.write('Invalid response from circud');
		return;
	}

	let head = substr(raw, 0, boundary[0]);
	let body = substr(raw, boundary[0] + boundary[1]);
	let lines = split(replace(head, '\r\n', '\n'), '\n');
	let status = match(lines[0], /^HTTP\/1\.[01] ([0-9]{3}) ?(.*)$/);
	if (!status) {
		http.status(502, 'Bad Gateway');
		http.write('Invalid response from circud');
		return;
	}

	let status_code = +status[1], declared_length = null;
	let response_headers = [], session_cookie = null;
	for (let i = 1; i < length(lines); i++) {
		let header = match(lines[i], /^([^:]+):[ \t]*(.*)$/);
		if (!header)
			continue;
		let name = lc(header[1]);
		if (name == 'content-length') {
			if (match(header[2], /^[0-9]+$/))
				declared_length = header[2];
			continue;
		}
		if (name == 'set-cookie') {
			let parsed = upstream_session_cookie(header[2]);
			if (parsed) {
				if (session_cookie != null) {
					http.status(502, 'Bad Gateway');
					http.write('Invalid duplicate session cookie from circud');
					return;
				}
				session_cookie = parsed;
			}
			continue;
		}
		if (index(HOP_HEADERS, name) >= 0 ||
			name == 'content-security-policy' || name == 'x-frame-options' ||
			name == 'set-cookie2')
			continue;
		push(response_headers, [ header[1], header[2] ]);
	}

	http.status(status_code, status[2] || 'OK');
	for (let header in response_headers)
		http.header(header[0], header[1]);
	if (session_cookie)
		http.header('Set-Cookie',
			rewrite_session_cookie(session_cookie, cookie_path,
				http.getenv('HTTPS') == 'on'));

	let has_body = method != 'HEAD' && status_code >= 200 &&
		status_code != 204 && status_code != 304;
	if (method == 'HEAD' && declared_length != null)
		http.header('Content-Length', declared_length);
	else if (has_body)
		http.header('Content-Length', '' + length(body));
	http.header('Content-Security-Policy', "default-src 'self'; " +
		"script-src 'self'; style-src 'self'; img-src 'self' data:; " +
		"connect-src 'self'; frame-ancestors 'self'; base-uri 'self'; " +
		"form-action 'self'");
	http.write(has_body ? body : '');
}

return {
	// Kept side-effect free so the native ucode runtime can exercise the
	// credential boundary without opening a listener or touching /etc.
	__test: {
		valid_token,
		parse_session_cookie,
		upstream_session_cookie,
		rewrite_session_cookie
	},
	proxy: function(env, ...segments) {
		http = env.http;
		if (!http)
			return;
		let access_groups = env.ctx?.authacl?.['access-group'];
		if (index(access_groups?.['luci-app-circud'] || [], 'write') < 0) {
			http.status(403, 'Forbidden');
			http.write('Write permission for circud is required');
			return;
		}

		let destination = target();
		if (!destination) {
			http.status(503, 'Service Unavailable');
			http.write('circud Web management is disabled or misconfigured');
			return;
		}
		let cookie_path = proxy_cookie_path(env);
		if (!cookie_path) {
			http.status(500, 'Internal Server Error');
			http.write('Unable to determine circud proxy cookie path');
			return;
		}
		let path_info = http.getenv('PATH_INFO') || '';
		if (!length(segments) && length(path_info) &&
			substr(path_info, -1) != '/') {
			http.status(302, 'Found');
			http.header('Location', cookie_path);
			http.header('Content-Length', '0');
			http.write('');
			return;
		}
		let session = request_session_cookie();
		if (session.invalid) {
			http.status(400, 'Bad Request');
			http.write('Invalid circud proxy session cookie');
			return;
		}

		let path = '/' + join('/', map(segments, part => http.urlencode(part)));
		if (path == '/') path = '/';
		if (!is_management_path(path)) {
			http.status(404, 'Not Found');
			http.write('The LuCI proxy exposes only circud management routes. ' +
				'Use the native circud listener for package endpoints, SSE, ' +
				'WebSocket, and extension HTTP methods.');
			return;
		}
		let upgrade = http.getenv('HTTP_UPGRADE');
		let accept = lc(http.getenv('HTTP_ACCEPT') || '');
		if ((upgrade && length(upgrade) > 0) ||
			index(accept, 'text/event-stream') >= 0) {
			http.status(501, 'Not Implemented');
			http.write('The LuCI management proxy does not support SSE or ' +
				'protocol upgrades. Use the native circud listener.');
			return;
		}

		let method = http.getenv('REQUEST_METHOD') || 'GET';
		if (index([ 'GET', 'HEAD', 'POST' ], method) < 0) {
			http.status(405, 'Method Not Allowed');
			http.header('Allow', 'GET, HEAD, POST');
			http.write('Method not allowed');
			return;
		}

		let content_length = +(http.getenv('CONTENT_LENGTH') || 0);
		let raw_content_length = http.getenv('CONTENT_LENGTH');
		if ((raw_content_length && !match(raw_content_length, /^[0-9]+$/)) ||
			content_length < 0 || content_length > MAX_REQUEST_BYTES) {
			http.status(413, 'Payload Too Large');
			http.write('Request body is too large');
			return;
		}

		let query = http.getenv('QUERY_STRING');
		if (query && match(query, /[\r\n]/)) {
			http.status(400, 'Bad Request');
			http.write('Invalid query string');
			return;
		}
		if (query) path += '?' + query;

		let host = index(destination.address, ':') >= 0
			? '[' + destination.address + ']'
			: destination.address;
		let request = sprintf('%s %s HTTP/1.0\r\nHost: %s:%d\r\n' +
			'Connection: close\r\n', method, path, host, destination.port);
		let content_type = http.getenv('CONTENT_TYPE');
		if (content_type)
			request += sprintf('Content-Type: %s\r\n',
				replace(content_type, /[\r\n]/g, ''));
		if (content_length)
			request += sprintf('Content-Length: %d\r\n', content_length);
		if (session.token)
			request += 'Cookie: ' + UPSTREAM_COOKIE + '=' +
				session.token + '\r\n';
		let csrf = http.getenv('HTTP_X_CIRCUD_CSRF');
		if (csrf) {
			if (!valid_token(csrf)) {
				http.status(400, 'Bad Request');
				http.write('Invalid X-Circud-CSRF header');
				return;
			}
			request += 'X-Circud-CSRF: ' + csrf + '\r\n';
		}
		request += '\r\n';

		let sock = socket.connect(destination.address, destination.port, {
			socktype: socket.SOCK_STREAM,
			flags: socket.AI_NUMERICHOST
		}, 5000);
		if (sock) {
			sock.setopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO,
				{ sec: IO_TIMEOUT_MS / 1000, usec: 0 });
			sock.setopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO,
				{ sec: IO_TIMEOUT_MS / 1000, usec: 0 });
		}
		if (!sock || !send_all(sock, request)) {
			if (sock) sock.close();
			http.status(502, 'Bad Gateway');
			http.write('Unable to connect to circud');
			return;
		}

		let remaining = content_length;
		while (remaining > 0) {
			let chunk = http.input();
			if (chunk == null || length(chunk) == 0 ||
				length(chunk) > remaining || !send_all(sock, chunk)) {
				sock.close();
				http.status(400, 'Bad Request');
				http.write('Incomplete request body');
				return;
			}
			remaining -= length(chunk);
		}

		let raw = read_response(sock);
		sock.close();
		if (raw == null) {
			http.status(502, 'Bad Gateway');
			http.write('Unable to read circud response');
			return;
		}

		proxy_response(raw, method, cookie_path);
	}
};
