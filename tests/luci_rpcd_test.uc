import { open } from 'fs';

let source_path = ARGV[0];
let config_path = ARGV[1];
let plugin = loadfile(source_path)();
let api = plugin?.['luci.circud'];

assert(type(api) == 'object', 'luci.circud rpc object is missing');
for (let method in [
	'status', 'read_config', 'read_config_private', 'write_config', 'hash_password',
	'service_action', 'proxy_access'
])
	assert(type(api[method]?.call) == 'function',
		'missing rpc method: ' + method);

function write_config_fixture(value) {
	let file = open(config_path, 'w', 0o600);
	assert(file, 'unable to open rpcd configuration fixture');
	assert(file.write(value) == length(value),
		'unable to write complete rpcd configuration fixture');
	file.close();
}

write_config_fixture('{"auth":{"enabled":false,"username":"admin",' +
	'"passwordHash":"private-hash-value"}}\n');
let public_config = api.read_config.call();
let private_config = api.read_config_private.call();
assert(public_config.exists == true && !public_config.error,
	'read-only configuration failed');
assert(json(public_config.content).auth.passwordHash == '<redacted>',
	'read-only configuration exposed the authentication hash');
assert(private_config.exists == true && !private_config.error,
	'private configuration failed');
assert(json(private_config.content).auth.passwordHash == 'private-hash-value',
	'write-authorized configuration did not preserve the authentication hash');

write_config_fixture('{"listen":{"port":1234}}\n');
let sparse_config = api.read_config.call();
assert(sparse_config.exists == true && !sparse_config.error &&
	json(sparse_config.content).listen.port == 1234,
	'read-only access rejected a valid sparse configuration');

let missing_content = api.write_config.call({ args: {} });
assert(missing_content.success == false &&
	index(missing_content.error, 'string') >= 0,
	'write_config accepted a missing content value');

let empty_content = api.write_config.call({ args: { content: '' } });
assert(empty_content.success == false &&
	index(empty_content.error, 'must not be empty') >= 0,
	'write_config accepted an empty configuration');

let missing_password = api.hash_password.call({ args: {} });
assert(missing_password.success == false &&
	index(missing_password.error, 'string') >= 0,
	'hash_password accepted a missing value');

let empty_password = api.hash_password.call({ args: { password: '' } });
assert(empty_password.success == false &&
	index(empty_password.error, '1..1024') >= 0,
	'hash_password accepted an empty value');

let multiline_password = api.hash_password.call({
	args: { password: 'first\nsecond' }
});
assert(multiline_password.success == false &&
	index(multiline_password.error, 'single line') >= 0,
	'hash_password accepted a multiline value');

let arbitrary_action = api.service_action.call({
	args: { action: 'start; touch /tmp/not-allowed' }
});
assert(arbitrary_action.success == false &&
	index(arbitrary_action.error, 'unsupported') >= 0,
	'service_action accepted an action outside its fixed allowlist');

assert(api.proxy_access.call().allowed == true,
	'proxy_access marker returned an unexpected value');

print('LuCI rpcd validation tests passed\n');
