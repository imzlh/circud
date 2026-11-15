'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

const overviewPath = path.join(__dirname, '..', 'packaging', 'openwrt',
	'luci-app-circud', 'htdocs', 'luci-static', 'resources', 'view',
	'circud', 'overview.js');
const overview = require(overviewPath);
const {
	DEFAULT_CONFIG,
	mergeKnownConfig,
	formData,
	restoreFormData,
	configFromForm,
	validateConfigRelations
} = overview;

function leafPaths(value, prefix = '') {
	const result = [];
	for (const [ key, item ] of Object.entries(value)) {
		const current = prefix ? `${prefix}.${key}` : key;
		if (item !== null && typeof item === 'object' && !Array.isArray(item))
			result.push(...leafPaths(item, current));
		else
			result.push(current);
	}
	return result.sort();
}

const exampleConfig = JSON.parse(fs.readFileSync(path.join(__dirname, '..',
	'config.example.json'), 'utf8'));
const openwrtConfig = JSON.parse(fs.readFileSync(path.join(__dirname, '..',
	'packaging', 'openwrt', 'circud', 'config.json'), 'utf8'));

assert.deepEqual(leafPaths(DEFAULT_CONFIG), leafPaths(exampleConfig),
	'the LuCI schema must track every field in the canonical example');
assert.deepEqual(leafPaths(DEFAULT_CONFIG), leafPaths(openwrtConfig),
	'the LuCI schema must track every field in the packaged OpenWrt config');

assert.deepEqual(configFromForm(formData(DEFAULT_CONFIG)), DEFAULT_CONFIG,
	'the graphical form must round-trip every default field');
assert.deepEqual(leafPaths(configFromForm(formData(DEFAULT_CONFIG))),
	leafPaths(DEFAULT_CONFIG), 'the graphical form must cover every JSON leaf');

const sparse = mergeKnownConfig(DEFAULT_CONFIG, {
	listen: { port: 1234 },
	web: { enabled: false }
});
assert.equal(sparse.listen.port, 1234);
assert.equal(sparse.listen.address, DEFAULT_CONFIG.listen.address);
assert.equal(sparse.web.enabled, false);
assert.equal(sparse.web.publicDir, DEFAULT_CONFIG.web.publicDir);

assert.throws(() => mergeKnownConfig(DEFAULT_CONFIG, { unexpected: true }),
	/Unknown configuration field: unexpected/);
assert.throws(() => mergeKnownConfig(DEFAULT_CONFIG,
	{ web: { unexpected: true } }),
	/Unknown configuration field: web\.unexpected/);
assert.throws(() => mergeKnownConfig(DEFAULT_CONFIG, { web: false }),
	/web must be an object/);
assert.throws(() => mergeKnownConfig(DEFAULT_CONFIG,
	{ listen: { port: '9080' } }), /listen\.port must be a number/);

const stringData = formData(DEFAULT_CONFIG);
stringData.web.enabled = 'false';
stringData.listen.port = '12345';
const converted = configFromForm(stringData);
assert.equal(converted.web.enabled, false);
assert.equal(converted.listen.port, 12345);

const failedFormData = formData(DEFAULT_CONFIG);
failedFormData.general['.name'] = 'general';
failedFormData.general.stateDir = '/failed-save';
failedFormData.auth._newPassword = 'must-not-survive-reset';
restoreFormData(failedFormData, DEFAULT_CONFIG);
assert.deepEqual(configFromForm(failedFormData), DEFAULT_CONFIG,
	'Reset must restore the last successfully saved configuration');
assert.equal(failedFormData.general['.name'], 'general',
	'Reset must retain JSONMap section metadata');
assert.equal(Object.hasOwn(failedFormData.auth, '_newPassword'), false,
	'Reset must discard transient fields from a failed save');

assert.throws(() => validateConfigRelations({
	...DEFAULT_CONFIG,
	auth: { ...DEFAULT_CONFIG.auth, sessionIdleTimeoutMs: 2000,
		sessionMaxLifetimeMs: 1000 }
}), /idle timeout/);
assert.throws(() => validateConfigRelations({
	...DEFAULT_CONFIG,
	endpoints: { ...DEFAULT_CONFIG.endpoints, maxSessions: 1,
		maxSessionsPerPackage: 2 }
}), /sessions per package/);
assert.throws(() => validateConfigRelations({
	...DEFAULT_CONFIG,
	endpoints: { ...DEFAULT_CONFIG.endpoints, maxMessageBytes: 2048,
		maxQueuedBytes: 1024 }
}), /message size/);
assert.throws(() => validateConfigRelations({
	...DEFAULT_CONFIG,
	endpoints: { ...DEFAULT_CONFIG.endpoints, idleTimeoutMs: 2000,
		maxLifetimeMs: 1000 }
}), /idle timeout/);

const source = fs.readFileSync(overviewPath, 'utf8');
const configSource = fs.readFileSync(path.join(__dirname, '..', 'src',
	'config.cpp'), 'utf8');

function acceptedCoreFieldPaths(sourceText) {
	const records = [];
	for (const match of sourceText.matchAll(
		/reject_unknown\(\s*\w+,\s*\{([\s\S]*?)\},\s*"([^"]*)"\s*\);/g)) {
		const names = Array.from(match[1].matchAll(/"([^"]+)"/g),
			item => item[1]);
		records.push({ scope: match[2], names });
	}

	const nested = new Set(records.map(record => record.scope)
		.filter(scope => scope));
	const paths = [];
	for (const record of records) {
		for (const name of record.names) {
			if (!record.scope && nested.has(name))
				continue;
			paths.push(record.scope ? `${record.scope}.${name}` : name);
		}
	}
	return paths.sort();
}

assert.deepEqual(acceptedCoreFieldPaths(configSource), leafPaths(DEFAULT_CONFIG),
	'the LuCI schema must cover every field accepted by the core parser');

function graphicalFieldPaths(sourceText) {
	const start = sourceText.indexOf('\tbuildForm: function(config) {');
	const end = sourceText.indexOf('\n\trender: function(data) {', start);
	assert.notEqual(start, -1, 'buildForm() must exist');
	assert.notEqual(end, -1, 'buildForm() must end before render()');

	const fields = [];
	let section = null;
	const tokens = sourceText.slice(start, end).matchAll(
		/map\.section\(form\.NamedSection,\s*'([^']+)'|add(?:Value|Flag)\(section,\s*'([^']+)'/gs);
	for (const token of tokens) {
		if (token[1]) {
			section = token[1];
			continue;
		}

		assert.ok(section, `form field ${token[2]} must belong to a section`);
		if (token[2].startsWith('_'))
			continue;
		fields.push(section === 'general'
			? token[2]
			: `${section}.${token[2]}`);
	}

	return fields.sort();
}

assert.deepEqual(graphicalFieldPaths(source), leafPaths(DEFAULT_CONFIG),
	'every configuration leaf must have a graphical LuCI control');
assert.doesNotMatch(source, /form\.TextValue|<textarea|JSON document/i,
	'the LuCI page must not fall back to raw JSON editing');
assert.match(source, /map\.tabbed\s*=\s*true/,
	'the complete form should be split into tabs');
assert.match(source,
	/function addValue[\s\S]*?option\.rmempty\s*=\s*false[\s\S]*?return option;/,
	'graphical value controls must reject empty required values');
assert.match(source,
	/function addFlag[\s\S]*?option\.rmempty\s*=\s*false[\s\S]*?return option;/,
	'graphical flags must always preserve an explicit boolean value');
assert.match(source, /map\.readonly\s*=\s*!this\.writable/,
	'the entire form must inherit the LuCI write permission');
assert.match(source,
	/const clearPasswords\s*=\s*function\(\)\s*\{[\s\S]*?newPassword\s*=\s*''[\s\S]*?confirmation\s*=\s*''[\s\S]*?passwordOption[\s\S]*?confirmationOption[\s\S]*?setValue\(''\)[\s\S]*?\};/,
	'plaintext password values and widgets must share one cleanup path');
assert.match(source,
	/if \(newPassword !== confirmation\)[\s\S]*?\.finally\(clearPasswords\)/,
	'plaintext password fields must be cleared after confirmation failure');
assert.match(source,
	/return dom\.callClassMethod\([\s\S]*?\.finally\(clearPasswords\);/,
	'plaintext password fields must be cleared after all later save outcomes');
assert.match(source,
	/handleReset:\s*function\(\)\s*\{\s*restoreFormData\(this\.formMap\.data\.data, this\.savedConfig\);\s*return this\.formMap\.reset\(\);\s*\}/,
	'Reset must restore the last successful snapshot before re-rendering');
assert.match(source,
	/L\.url\(['"]admin\/services\/circud\/web['"]\)\s*\+\s*['"]\/['"]/,
	'the management link must retain a trailing slash for relative assets');
assert.doesNotMatch(source,
	/L\.url\(['"]admin\/services\/circud\/web\/['"]\)/,
	'LuCI rejects URL parts ending in a slash and would link to its root');

console.log('LuCI overview configuration tests passed');
