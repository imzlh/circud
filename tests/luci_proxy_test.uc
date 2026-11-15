import { open } from 'fs';

let source_path = ARGV[0];
let mode = ARGV[1];
let headers = {}, status = null, body = '';
let request_body = null;
let environment = {
	SCRIPT_NAME: '/cgi-bin/luci',
	PATH_INFO: '/admin/services/circud/web/',
	REQUEST_METHOD: 'GET',
	CONTENT_LENGTH: '0',
	QUERY_STRING: '',
	HTTPS: 'on'
};

if (mode == 'readonly')
	environment.HTTP_COOKIE = 'sysauth_https=secret';
else if (mode == 'invalid-cookie')
	environment.HTTP_COOKIE = 'luci_circud_session=short';
else if (mode == 'canonical-path')
	environment.PATH_INFO = '/admin/services/circud/web';
else if (mode == 'unknown-api')
	environment.PATH_INFO += 'api/future-core-route';
else if (mode == 'package-endpoint')
	environment.PATH_INFO += 'demo/health';
else if (mode == 'upstream-get') {
	environment.PATH_INFO += 'app.js';
	environment.QUERY_STRING = 'cache=1';
	environment.HTTP_COOKIE = 'sysauth_https=luci-secret; theme=dark';
}
else if (mode == 'upstream-rpc') {
	request_body = '{"method":"system.info","params":{}}';
	environment.PATH_INFO += 'rpc';
	environment.REQUEST_METHOD = 'POST';
	environment.CONTENT_LENGTH = '' + length(request_body);
	environment.CONTENT_TYPE = 'application/json';
	environment.HTTP_COOKIE = 'sysauth_https=luci-secret; ' +
		'luci_circud_session=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA';
	environment.HTTP_X_CIRCUD_CSRF =
		'BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB';
}
else if (mode == 'upstream-upload') {
	request_body = 'PK\x03\x04circud-test-archive';
	environment.PATH_INFO += 'api/packages/install';
	environment.REQUEST_METHOD = 'POST';
	environment.CONTENT_LENGTH = '' + length(request_body);
	environment.CONTENT_TYPE = 'application/zip';
	environment.QUERY_STRING = 'replace=1';
	environment.HTTP_COOKIE = 'sysauth_https=luci-secret; ' +
		'luci_circud_session=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA';
	environment.HTTP_X_CIRCUD_CSRF =
		'BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB';
}

let input_offset = 0;

let http = {
	getenv: name => environment[name],
	status: (code, reason) => { status = [ code, reason ]; },
	header: (name, value) => { headers[name] = value; },
	write: value => { body += value || ''; },
	urlencode: value => value,
	input: () => {
		if (request_body == null || input_offset >= length(request_body))
			return null;
		let chunk = substr(request_body, input_offset, 7);
		input_offset += length(chunk);
		return chunk;
	}
};

let controller = loadfile(source_path)();
let groups = mode == 'readonly' ? {} : { 'luci-app-circud': [ 'write' ] };
let segments = [];
if (mode == 'upstream-get')
	segments = [ 'app.js' ];
else if (mode == 'upstream-rpc')
	segments = [ 'rpc' ];
else if (mode == 'upstream-upload')
	segments = [ 'api', 'packages', 'install' ];
else if (mode == 'unknown-api')
	segments = [ 'api', 'future-core-route' ];
else if (mode == 'package-endpoint')
	segments = [ 'demo', 'health' ];

controller.proxy({
	http,
	ctx: {
		authacl: { 'access-group': groups },
		path: [ 'admin', 'services', 'circud', 'web' ]
	}
}, ...segments);

if (mode == 'readonly') {
	assert(status[0] == 403, 'read-only access was not rejected');
	assert(index(body, 'Write permission') >= 0,
		'missing read-only error body');
}
else if (mode == 'invalid-cookie') {
	assert(status[0] == 400, 'malformed proxy cookie was not rejected');
	assert(index(body, 'Invalid circud proxy session cookie') >= 0,
		'missing malformed-cookie error body');
}
else if (mode == 'canonical-path') {
	assert(status[0] == 302, 'slashless proxy root was not redirected');
	assert(headers.Location ==
		'/cgi-bin/luci/admin/services/circud/web/',
		'proxy root redirect did not preserve the LuCI script prefix');
	assert(headers['Content-Length'] == '0',
		'proxy root redirect unexpectedly has a response body');
}
else if (mode == 'unknown-api' || mode == 'package-endpoint') {
	assert(status[0] == 404,
		'non-management route was not rejected by the LuCI proxy');
	assert(index(body, 'only circud management routes') >= 0,
		'missing management-boundary error body');
}
else if (mode == 'credentials') {
	let helpers = controller.__test;
	let token = 'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA';
	let csrf = 'BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB';

	assert(helpers.valid_token(token), 'valid session token was rejected');
	assert(helpers.valid_token(csrf), 'valid CSRF token was rejected');
	assert(!helpers.valid_token('short'), 'short token was accepted');
	assert(!helpers.valid_token(token + '='), 'invalid token alphabet was accepted');

	let luci_only = helpers.parse_session_cookie(
		'sysauth_https=secret; sysauth=another-secret');
	assert(luci_only.token == null && !luci_only.invalid,
		'LuCI session cookie was treated as a circud session');

	let mapped = helpers.parse_session_cookie(
		'sysauth_https=secret; luci_circud_session=' + token +
		'; unrelated=value');
	assert(mapped.token == token && !mapped.invalid,
		'circud proxy session was not isolated from other cookies');
	let duplicate = helpers.parse_session_cookie(
		'luci_circud_session=' + token + '; luci_circud_session=' + token);
	assert(duplicate.invalid, 'duplicate circud proxy cookies were accepted');

	let upstream = helpers.upstream_session_cookie(
		'circud_session=' + token +
		'; Path=/; HttpOnly; SameSite=Strict; Max-Age=60');
	assert(upstream.token == token && upstream.max_age == 60,
		'valid upstream circud session cookie was rejected');
	assert(helpers.upstream_session_cookie(
		'sysauth_https=secret; Path=/; HttpOnly; SameSite=Strict; Max-Age=60') == null,
		'non-circud upstream cookie was accepted');
	assert(helpers.upstream_session_cookie(
		'circud_session=' + token +
		'; Path=/; HttpOnly; SameSite=Lax; Max-Age=60') == null,
		'weakened upstream SameSite policy was accepted');

	let rewritten = helpers.rewrite_session_cookie(upstream,
		'/cgi-bin/luci/admin/services/circud/web/', true);
	assert(index(rewritten, 'luci_circud_session=' + token) == 0,
		'upstream session was not renamed');
	assert(index(rewritten,
		'Path=/cgi-bin/luci/admin/services/circud/web/') >= 0,
		'proxy session was not path limited');
	assert(index(rewritten, '; HttpOnly; SameSite=Strict; Secure;') >= 0,
		'proxy session security attributes are incomplete');
	assert(index(rewritten, 'sysauth') < 0,
		'LuCI session name leaked into rewritten cookie');
}
else if (mode == 'upstream-get') {
	assert(status[0] == 200, 'proxied static request failed');
	assert(body == 'proxied-app-js', 'proxied static body changed');
	assert(headers['Content-Type'] == 'application/javascript',
		'proxied content type was lost');
	assert(headers['X-Circud-Test'] == 'static',
		'ordinary upstream response header was lost');
	assert(headers.Connection == null,
		'hop-by-hop response header leaked through proxy');
	assert(index(headers['Content-Security-Policy'] || '',
		"default-src 'self'") >= 0,
		'proxy did not replace upstream content security policy');
}
else if (mode == 'upstream-rpc') {
	assert(status[0] == 200, 'proxied RPC request failed');
	assert(body == '{"ok":true,"result":{"version":"test"}}',
		'proxied RPC response body changed');
	assert(headers['Set-Cookie'] ==
		'luci_circud_session=CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC; ' +
		'Path=/cgi-bin/luci/admin/services/circud/web/; HttpOnly; ' +
		'SameSite=Strict; Secure; Max-Age=60',
		'upstream circud session was not securely rewritten');
	assert(index(headers['Set-Cookie'], 'sysauth') < 0,
		'LuCI session name leaked into downstream cookie');
}
else if (mode == 'upstream-upload') {
	assert(status[0] == 201, 'proxied ZIP upload failed');
	assert(body == '{"ok":true,"result":{"installed":true}}',
		'proxied upload response body changed');
}
else {
	die('unknown test mode');
}

print('LuCI proxy ' + mode + ' test passed\n');
