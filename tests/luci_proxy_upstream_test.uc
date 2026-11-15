import * as socket from 'socket';

let mode = ARGV[0];
let listener = socket.listen('127.0.0.1', 9080, {
	family: socket.AF_INET,
	socktype: socket.SOCK_STREAM,
	flags: socket.AI_NUMERICHOST
}, 1, true);
assert(listener, 'unable to listen on the circud test port');
print('LuCI proxy upstream ready\n');

let peer = listener.accept();
assert(peer, 'unable to accept proxy connection');
listener.close();

peer.setopt(socket.SOL_SOCKET, socket.SO_RCVTIMEO, { sec: 10, usec: 0 });
peer.setopt(socket.SOL_SOCKET, socket.SO_SNDTIMEO, { sec: 10, usec: 0 });

let raw = '';
while (index(raw, '\r\n\r\n') < 0) {
	let chunk = peer.recv(4096);
	assert(chunk != null && length(chunk) > 0,
		'proxy closed before sending complete request headers');
	raw += chunk;
	assert(length(raw) <= 64 * 1024, 'proxy request headers are too large');
}

let boundary = index(raw, '\r\n\r\n');
let head = substr(raw, 0, boundary);
let header_block = '\r\n' + head + '\r\n';
let request_body = substr(raw, boundary + 4);
let content_length = 0;
for (let line in split(head, '\r\n')) {
	let matched = match(line, /^Content-Length: ([0-9]+)$/i);
	if (matched)
		content_length = +matched[1];
}
while (length(request_body) < content_length) {
	let chunk = peer.recv(content_length - length(request_body));
	assert(chunk != null && length(chunk) > 0,
		'proxy closed before sending complete request body');
	request_body += chunk;
}
assert(length(request_body) == content_length,
	'proxy sent more request body bytes than declared');

let response;
if (mode == 'upstream-get') {
	assert(index(head, 'GET /app.js?cache=1 HTTP/1.0\r\n') == 0,
		'static request target changed');
	assert(index(header_block, '\r\nHost: 127.0.0.1:9080\r\n') >= 0,
		'proxy sent an unexpected Host header');
	assert(index(header_block, '\r\nCookie:') < 0,
		'LuCI or unrelated cookie leaked upstream');
	assert(content_length == 0, 'GET request unexpectedly has a body');
	response = 'HTTP/1.0 200 OK\r\n' +
		'Content-Type: application/javascript\r\n' +
		'Content-Length: 14\r\n' +
		'Connection: keep-alive\r\n' +
		'Content-Security-Policy: default-src *\r\n' +
		'X-Circud-Test: static\r\n\r\nproxied-app-js';
}
else if (mode == 'upstream-rpc') {
	assert(index(head, 'POST /rpc HTTP/1.0\r\n') == 0,
		'RPC request target changed');
	assert(index(header_block, '\r\nContent-Type: application/json\r\n') >= 0,
		'RPC content type was not forwarded');
	assert(index(header_block, '\r\nCookie: circud_session=' +
		'AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA\r\n') >= 0,
		'isolated circud session was not mapped upstream');
	assert(index(header_block, 'sysauth') < 0,
		'LuCI session cookie leaked upstream');
	assert(index(header_block, '\r\nX-Circud-CSRF: ' +
		'BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB\r\n') >= 0,
		'validated circud CSRF token was not forwarded');
	assert(request_body == '{"method":"system.info","params":{}}',
		'RPC request body changed');
	response = 'HTTP/1.0 200 OK\r\n' +
		'Content-Type: application/json\r\n' +
		'Set-Cookie: circud_session=' +
		'CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC; Path=/; ' +
		'HttpOnly; SameSite=Strict; Max-Age=60\r\n' +
		'Set-Cookie: ignored=value; Path=/\r\n' +
		'Content-Length: 39\r\n\r\n' +
		'{"ok":true,"result":{"version":"test"}}';
}
else if (mode == 'upstream-upload') {
	assert(index(head,
		'POST /api/packages/install?replace=1 HTTP/1.0\r\n') == 0,
		'upload request target changed');
	assert(index(header_block, '\r\nContent-Type: application/zip\r\n') >= 0,
		'upload content type was not forwarded');
	assert(index(header_block, 'sysauth') < 0,
		'LuCI session cookie leaked during upload');
	assert(request_body == 'PK\x03\x04circud-test-archive',
		'ZIP upload body changed');
	response = 'HTTP/1.0 201 Created\r\n' +
		'Content-Type: application/json\r\n' +
		'Content-Length: 39\r\n\r\n' +
		'{"ok":true,"result":{"installed":true}}';
}
else {
	die('unknown upstream test mode');
}

let offset = 0;
while (offset < length(response)) {
	let written = peer.send(substr(response, offset));
	assert(written != null && written > 0,
		'unable to send complete upstream test response');
	offset += written;
}
peer.close();
print('LuCI proxy upstream ' + mode + ' test passed\n');
