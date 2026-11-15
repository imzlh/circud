'use strict';
'require view';
'require rpc';
'require poll';
'require ui';
'require form';
'require dom';

const IS_NODE = typeof module == 'object' && module.exports;

const callStatus = IS_NODE ? null : rpc.declare({
	object: 'luci.circud',
	method: 'status',
	expect: { '': {} }
});

const callReadConfig = IS_NODE ? null : rpc.declare({
	object: 'luci.circud',
	method: 'read_config',
	expect: { '': {} }
});

const callReadConfigPrivate = IS_NODE ? null : rpc.declare({
	object: 'luci.circud',
	method: 'read_config_private',
	expect: { '': {} }
});

const callWriteConfig = IS_NODE ? null : rpc.declare({
	object: 'luci.circud',
	method: 'write_config',
	params: [ 'content' ],
	expect: { '': {} }
});

const callHashPassword = IS_NODE ? null : rpc.declare({
	object: 'luci.circud',
	method: 'hash_password',
	params: [ 'password' ],
	expect: { '': {} }
});

const callServiceAction = IS_NODE ? null : rpc.declare({
	object: 'luci.circud',
	method: 'service_action',
	params: [ 'action' ],
	expect: { '': {} }
});

const callSessionAccess = IS_NODE ? null : rpc.declare({
	object: 'session',
	method: 'access',
	params: [ 'scope', 'object', 'function' ],
	expect: { 'access': false }
});

const DEFAULT_CONFIG = {
	stateDir: '/var/lib/circud',
	listen: { address: '127.0.0.1', port: 9080 },
	web: {
		enabled: true,
		publicDir: '/usr/share/circud/public',
		backgroundWorkers: 2,
		maxConnections: 128,
		ioTimeoutMs: 30000,
		maxBodyBytes: 65536,
		maxUploadBytes: 67108864,
		maxStaticFileBytes: 4194304
	},
	status: { enabled: true, intervalMs: 30000, timeoutMs: 5000 },
	calls: { defaultTimeoutMs: 30000 },
	runtimeStartTimeoutMs: 10000,
	events: { enabled: true, capacity: 256 },
	auth: {
		enabled: false,
		username: 'admin',
		passwordHash: '',
		sessionIdleTimeoutMs: 1800000,
		sessionMaxLifetimeMs: 86400000,
		maxSessions: 64,
		secureCookie: false
	},
	endpoints: {
		enabled: true,
		maxSessions: 128,
		maxSessionsPerPackage: 32,
		maxRequestBodyBytes: 1048576,
		maxResponseBodyBytes: 4194304,
		maxMessageBytes: 262144,
		maxQueuedMessages: 64,
		maxQueuedBytes: 1048576,
		idleTimeoutMs: 60000,
		maxLifetimeMs: 86400000
	}
};

function messageOf(error) {
	return error instanceof Error ? error.message : String(error);
}

function clone(value) {
	return JSON.parse(JSON.stringify(value));
}

function isPlainObject(value) {
	return value !== null && typeof value === 'object' &&
		!Array.isArray(value);
}

function mergeKnownConfig(defaults, supplied, path) {
	path = path || '';
	if (!isPlainObject(supplied))
		throw new Error(`${path || 'configuration'} must be an object`);

	for (const key in supplied) {
		if (!Object.prototype.hasOwnProperty.call(defaults, key))
			throw new Error(`Unknown configuration field: ${
				path ? path + '.' + key : key}`);
	}

	const merged = {};
	for (const key in defaults) {
		const fieldPath = path ? path + '.' + key : key;
		if (!Object.prototype.hasOwnProperty.call(supplied, key)) {
			merged[key] = clone(defaults[key]);
			continue;
		}

		const expected = defaults[key];
		const actual = supplied[key];
		if (isPlainObject(expected)) {
			merged[key] = mergeKnownConfig(expected, actual, fieldPath);
		}
		else if (typeof actual !== typeof expected ||
			actual === null || Array.isArray(actual)) {
			throw new Error(`${fieldPath} must be a ${typeof expected}`);
		}
		else {
			merged[key] = actual;
		}
	}

	return merged;
}

function formData(config) {
	return {
		general: {
			stateDir: config.stateDir,
			runtimeStartTimeoutMs: config.runtimeStartTimeoutMs
		},
		listen: clone(config.listen),
		web: clone(config.web),
		status: clone(config.status),
		calls: clone(config.calls),
		events: clone(config.events),
		auth: clone(config.auth),
		endpoints: clone(config.endpoints)
	};
}

function restoreFormData(data, config) {
	const restored = formData(config);
	for (const section in restored) {
		const current = data[section];
		if (!isPlainObject(current))
			throw new Error(`Missing form section: ${section}`);
		for (const key in current) {
			if (key.charAt(0) !== '.')
				delete current[key];
		}
		Object.assign(current, restored[section]);
	}
	return data;
}

function booleanValue(value) {
	return value === true || value === 'true' || value === '1';
}

function numberValue(value) {
	return Number(value);
}

function configFromForm(data) {
	return {
		stateDir: data.general.stateDir,
		runtimeStartTimeoutMs:
			numberValue(data.general.runtimeStartTimeoutMs),
		listen: {
			address: data.listen.address,
			port: numberValue(data.listen.port)
		},
		web: {
			enabled: booleanValue(data.web.enabled),
			publicDir: data.web.publicDir,
			backgroundWorkers: numberValue(data.web.backgroundWorkers),
			maxConnections: numberValue(data.web.maxConnections),
			ioTimeoutMs: numberValue(data.web.ioTimeoutMs),
			maxBodyBytes: numberValue(data.web.maxBodyBytes),
			maxUploadBytes: numberValue(data.web.maxUploadBytes),
			maxStaticFileBytes: numberValue(data.web.maxStaticFileBytes)
		},
		status: {
			enabled: booleanValue(data.status.enabled),
			intervalMs: numberValue(data.status.intervalMs),
			timeoutMs: numberValue(data.status.timeoutMs)
		},
		calls: {
			defaultTimeoutMs: numberValue(data.calls.defaultTimeoutMs)
		},
		events: {
			enabled: booleanValue(data.events.enabled),
			capacity: numberValue(data.events.capacity)
		},
		auth: {
			enabled: booleanValue(data.auth.enabled),
			username: data.auth.username,
			passwordHash: data.auth.passwordHash,
			sessionIdleTimeoutMs: numberValue(data.auth.sessionIdleTimeoutMs),
			sessionMaxLifetimeMs: numberValue(data.auth.sessionMaxLifetimeMs),
			maxSessions: numberValue(data.auth.maxSessions),
			secureCookie: booleanValue(data.auth.secureCookie)
		},
		endpoints: {
			enabled: booleanValue(data.endpoints.enabled),
			maxSessions: numberValue(data.endpoints.maxSessions),
			maxSessionsPerPackage:
				numberValue(data.endpoints.maxSessionsPerPackage),
			maxRequestBodyBytes:
				numberValue(data.endpoints.maxRequestBodyBytes),
			maxResponseBodyBytes:
				numberValue(data.endpoints.maxResponseBodyBytes),
			maxMessageBytes: numberValue(data.endpoints.maxMessageBytes),
			maxQueuedMessages: numberValue(data.endpoints.maxQueuedMessages),
			maxQueuedBytes: numberValue(data.endpoints.maxQueuedBytes),
			idleTimeoutMs: numberValue(data.endpoints.idleTimeoutMs),
			maxLifetimeMs: numberValue(data.endpoints.maxLifetimeMs)
		}
	};
}

function validateConfigRelations(config) {
	if (config.auth.sessionIdleTimeoutMs >
		config.auth.sessionMaxLifetimeMs)
		throw new Error('Authentication idle timeout must not exceed maximum lifetime.');
	if (config.endpoints.maxSessionsPerPackage >
		config.endpoints.maxSessions)
		throw new Error('Application sessions per package must not exceed total application sessions.');
	if (config.endpoints.maxMessageBytes >
		config.endpoints.maxQueuedBytes)
		throw new Error('Maximum application message size must not exceed queued bytes.');
	if (config.endpoints.idleTimeoutMs >
		config.endpoints.maxLifetimeMs)
		throw new Error('Application idle timeout must not exceed maximum lifetime.');
	return config;
}

function addValue(section, name, title, datatype, description) {
	const option = section.option(form.Value, name, title, description);
	option.rmempty = false;
	if (datatype)
		option.datatype = datatype;
	return option;
}

function addFlag(section, name, title, description) {
	const option = section.option(form.Flag, name, title, description);
	option.enabled = 'true';
	option.disabled = 'false';
	option.rmempty = false;
	return option;
}

const exported = {
	DEFAULT_CONFIG,
	mergeKnownConfig,
	formData,
	restoreFormData,
	configFromForm,
	validateConfigRelations
};

if (IS_NODE) {
	module.exports = exported;
}
else return view.extend({
	load: function() {
		const access = callSessionAccess(
			'ubus', 'luci.circud', 'proxy_access');
		return Promise.all([ callStatus(), access.then(function(writable) {
			return writable ? callReadConfigPrivate() : callReadConfig();
		}), access ]);
	},

	renderStatus: function(status) {
		const running = status.running === true;
		const enabled = status.enabled === true;
		const readonly = !this.writable;

		return E('div', { 'class': 'cbi-section' }, [
			E('h3', {}, _('Service status')),
			E('div', { 'class': 'table' }, [
				E('div', { 'class': 'tr' }, [
					E('div', { 'class': 'td left', 'width': '33%' }, _('Runtime')),
					E('div', { 'class': 'td left' }, [
						E('span', {
							'class': 'label %s'.format(running ? 'success' : 'warning')
						}, running ? _('Running') : _('Stopped'))
					])
				]),
				E('div', { 'class': 'tr' }, [
					E('div', { 'class': 'td left', 'width': '33%' }, _('Start at boot')),
					E('div', { 'class': 'td left' }, enabled ? _('Enabled') : _('Disabled'))
				]),
				E('div', { 'class': 'tr' }, [
					E('div', { 'class': 'td left', 'width': '33%' }, _('Configuration')),
					E('div', { 'class': 'td left' }, '/etc/circud/config.json')
				])
			]),
			E('div', { 'class': 'cbi-page-actions' }, [
				E('button', {
					'class': 'cbi-button cbi-button-action',
					'disabled': readonly ? '' : null,
					'click': ui.createHandlerFn(this, 'handleServiceAction',
						running ? 'restart' : 'start')
				}, [ running ? _('Restart') : _('Start') ]), ' ',
				E('button', {
					'class': 'cbi-button cbi-button-negative',
					'disabled': !readonly && running ? null : '',
					'click': ui.createHandlerFn(this, 'handleServiceAction', 'stop')
				}, [ _('Stop') ]), ' ',
				E('button', {
					'class': 'cbi-button cbi-button-neutral',
					'disabled': readonly ? '' : null,
					'click': ui.createHandlerFn(this, 'handleServiceAction',
						enabled ? 'disable' : 'enable')
				}, [ enabled ? _('Disable autostart') : _('Enable autostart') ])
			])
		]);
	},

	renderManagement: function() {
		const webAvailable = this.status.running === true &&
			this.currentWebEnabled();

		return E('div', { 'class': 'cbi-section' }, [
			this.proxyAllowed && webAvailable ? E('p', {}, [
				E('a', {
					'class': 'cbi-button cbi-button-action',
					// L.url() rejects a part ending in '/', so append it after
					// constructing the validated LuCI route. The trailing slash is
					// required for circud's relative app.js/app.css/API URLs.
					'href': L.url('admin/services/circud/web') + '/',
					'target': '_blank',
					'rel': 'noopener'
				}, [ _('Open circud management') ])
			]) : E('p', { 'class': 'alert-message warning' }, [
				!this.proxyAllowed
					? _('Your LuCI account has read-only circud access. The application management interface requires write permission.')
					: _('Start circud and enable its Web server to open application management.')
			]),
			E('p', { 'class': 'cbi-value-description' }, [
				_('This proxy carries only circud\'s management UI and core routes. Package HTTP endpoints, SSE, WebSocket and extension methods require direct access to the native listener.')
			])
		]);
	},

	currentWebEnabled: function() {
		const option = this.webEnabledOption;
		const element = option?.getUIElement('web');
		if (element)
			return booleanValue(element.getValue());
		return this.savedConfig?.web?.enabled === true;
	},

	buildForm: function(config) {
		const data = formData(config);
		const map = new form.JSONMap(data, _('Configuration'),
			_('All settings are written to /etc/circud/config.json and validated by circud before they replace the active configuration.'));
		map.readonly = !this.writable;
		map.tabbed = true;

		let section = map.section(form.NamedSection, 'general', 'general',
			_('Storage'));
		addValue(section, 'stateDir', _('State directory'), null,
			_('Persistent database, installed applications and application data.'));
		addValue(section, 'runtimeStartTimeoutMs',
			_('Runtime start timeout (ms)'), 'range(100,300000)',
			_('Maximum time allowed for an application runtime to start.'));

		section = map.section(form.NamedSection, 'listen', 'listen',
			_('HTTP listener'));
		addValue(section, 'address', _('Listen address'), 'ipaddr("nomask")',
			_('Keep this on loopback unless an authenticating reverse proxy protects it.'));
		addValue(section, 'port', _('Listen port'), 'range(1,65535)');

		section = map.section(form.NamedSection, 'web', 'web',
			_('Management Web server'));
		this.webEnabledOption = addFlag(section, 'enabled',
			_('Enable Web server'));
		addValue(section, 'publicDir', _('Static files directory'));
		addValue(section, 'backgroundWorkers', _('Background workers'),
			'range(1,16)');
		addValue(section, 'maxConnections', _('Maximum connections'),
			'range(1,4096)');
		addValue(section, 'ioTimeoutMs', _('I/O timeout (ms)'),
			'range(100,300000)');
		addValue(section, 'maxBodyBytes', _('Maximum RPC body (bytes)'),
			'range(1024,65536)');
		addValue(section, 'maxUploadBytes', _('Maximum ZIP upload (bytes)'),
			'range(1024,67108864)');
		addValue(section, 'maxStaticFileBytes',
			_('Maximum static file (bytes)'), 'range(1024,67108864)');

		section = map.section(form.NamedSection, 'status', 'status',
			_('Status snapshots'));
		addFlag(section, 'enabled', _('Enable status polling'));
		addValue(section, 'intervalMs', _('Polling interval (ms)'),
			'range(1000,3600000)');
		addValue(section, 'timeoutMs', _('Polling timeout (ms)'),
			'range(100,300000)');

		section = map.section(form.NamedSection, 'calls', 'calls',
			_('Management calls'));
		addValue(section, 'defaultTimeoutMs', _('Default call timeout (ms)'),
			'range(1,300000)');

		section = map.section(form.NamedSection, 'events', 'events',
			_('Management events'));
		addFlag(section, 'enabled', _('Enable event journal'));
		addValue(section, 'capacity', _('Retained event count'),
			'range(32,4096)');

		section = map.section(form.NamedSection, 'auth', 'auth',
			_('circud authentication'));
		addFlag(section, 'enabled', _('Enable circud login'),
			_('This protects both direct access and the LuCI management proxy with circud\'s own login.'));
		addValue(section, 'username', _('Username'), 'maxlength(128)');
		const hash = addValue(section, 'passwordHash', _('Password hash'), null,
			_('Advanced: an existing circud scrypt hash. Set a new password below to replace it safely.'));
		this.passwordHashOption = hash;
		hash.password = true;
		hash.rmempty = true;
		hash.remove = function(sectionId) {
			return this.write(sectionId, '');
		};
		const password = addValue(section, '_newPassword', _('New password'), null,
			_('Leave empty to retain the current password hash. Plaintext is used only for this save operation.'));
		this.passwordOption = password;
		password.password = true;
		password.rmempty = true;
		password.retain = true;
		password.write = function() {};
		password.remove = function() {};
		const confirmation = addValue(section, '_confirmPassword',
			_('Confirm new password'));
		this.confirmationOption = confirmation;
		confirmation.password = true;
		confirmation.rmempty = true;
		confirmation.retain = true;
		confirmation.write = function() {};
		confirmation.remove = function() {};
		addValue(section, 'sessionIdleTimeoutMs', _('Session idle timeout (ms)'),
			'range(1000,86400000)');
		addValue(section, 'sessionMaxLifetimeMs', _('Session maximum lifetime (ms)'),
			'range(1000,2592000000)');
		addValue(section, 'maxSessions', _('Maximum sessions'),
			'range(1,4096)');
		addFlag(section, 'secureCookie', _('Secure cookie'),
			_('Enable only when direct circud access uses HTTPS.'));

		section = map.section(form.NamedSection, 'endpoints', 'endpoints',
			_('Application endpoints'));
		addFlag(section, 'enabled', _('Enable application endpoints'));
		addValue(section, 'maxSessions', _('Maximum sessions'),
			'range(1,4096)');
		addValue(section, 'maxSessionsPerPackage',
			_('Maximum sessions per application'), 'range(1,1024)');
		addValue(section, 'maxRequestBodyBytes',
			_('Maximum request body (bytes)'), 'range(1024,16777216)');
		addValue(section, 'maxResponseBodyBytes',
			_('Maximum response body (bytes)'), 'range(1024,67108864)');
		addValue(section, 'maxMessageBytes',
			_('Maximum message (bytes)'), 'range(1024,4194304)');
		addValue(section, 'maxQueuedMessages', _('Maximum queued messages'),
			'range(1,1024)');
		addValue(section, 'maxQueuedBytes', _('Maximum queued bytes'),
			'range(1024,67108864)');
		addValue(section, 'idleTimeoutMs', _('Idle timeout (ms)'),
			'range(1000,3600000)');
		addValue(section, 'maxLifetimeMs', _('Maximum lifetime (ms)'),
			'range(1000,604800000)');

		this.formMap = map;
		this.formData = data;
		return map;
	},

	render: function(data) {
		this.status = data[0] || {};
		const response = data[1] || {};
		if (response.error)
			throw new Error(response.error);

		let config;
		if (response.exists === false) {
			config = clone(DEFAULT_CONFIG);
		}
		else {
			try {
				config = JSON.parse(response.content || '');
			}
			catch (error) {
				throw new Error(_('Unable to parse /etc/circud/config.json: %s')
					.format(messageOf(error)));
			}
		}

		try {
			config = mergeKnownConfig(DEFAULT_CONFIG, config);
		}
		catch (error) {
			throw new Error(_('Invalid /etc/circud/config.json: %s')
				.format(messageOf(error)));
		}

		this.savedConfig = clone(config);
		this.proxyAllowed = data[2] === true;
		this.writable = L.hasViewPermission() === true;
		this.statusNode = this.renderStatus(this.status);
		const map = this.buildForm(config);
		this.managementNode = this.renderManagement();

		poll.add(L.bind(this.refreshStatus, this), 5);

		return map.render().then(L.bind(function(formNode) {
			return E([
				E('h2', {}, _('circud')),
				E('div', { 'class': 'cbi-map-descr' }, [
					_('This page manages the circud daemon and its configuration. Installed applications and their lifecycle remain managed by circud itself.')
				]),
				this.managementNode,
				this.statusNode,
				formNode
			]);
		}, this));
	},

	refreshStatus: function() {
		return callStatus().then(L.bind(function(status) {
			this.status = status || {};
			const replacement = this.renderStatus(this.status);
			this.statusNode.replaceWith(replacement);
			this.statusNode = replacement;
			const management = this.renderManagement();
			this.managementNode.replaceWith(management);
			this.managementNode = management;
		}, this)).catch(function() {});
	},

	handleServiceAction: function(action) {
		return callServiceAction(action).then(L.bind(function(result) {
			if (result.success !== true)
				throw new Error(result.error || _('Service action failed'));

			ui.addNotification(null, E('p', {}, _('Service action completed.')),
				'info');
			return this.refreshStatus();
		}, this)).catch(function(error) {
			ui.addNotification(_('Service action failed'),
				E('p', {}, messageOf(error)), 'error');
		});
	},

	saveForm: function() {
		const mapNode = document.querySelector('#maincontent .cbi-map');
		const passwordOption = this.passwordOption;
		const confirmationOption = this.confirmationOption;
		let newPassword = passwordOption.formvalue('auth') || '';
		let confirmation = confirmationOption.formvalue('auth') || '';
		const clearPasswords = function() {
			newPassword = '';
			confirmation = '';
			for (const option of [ passwordOption, confirmationOption ])
				option?.getUIElement('auth')?.setValue('');
		};
		if (newPassword !== confirmation)
			return Promise.reject(new Error(
				_('The new password and confirmation do not match.')))
				.finally(clearPasswords);

		return dom.callClassMethod(mapNode, 'save').then(L.bind(function() {
			if (!newPassword)
				return null;
			return callHashPassword(newPassword).then(function(result) {
				if (result.success !== true || !result.hash)
					throw new Error(result.error || _('Password hashing failed'));
				return result.hash;
			});
		}, this)).then(L.bind(function(generatedHash) {
			const config = validateConfigRelations(configFromForm(
				this.formMap.data.data));
			if (generatedHash)
				config.auth.passwordHash = generatedHash;
			if (config.auth.enabled && !config.auth.passwordHash)
				throw new Error(_('Set a password before enabling circud authentication.'));
			const content = JSON.stringify(config, null, 2) + '\n';
			return callWriteConfig(content).then(function(result) {
				if (result.success !== true)
					throw new Error(result.error || _('Configuration validation failed'));
				return config;
			}).then(L.bind(function(saved) {
				if (generatedHash) {
					this.formMap.data.data.auth.passwordHash = generatedHash;
					this.passwordHashOption.getUIElement('auth')
						?.setValue(generatedHash);
				}
				return saved;
			}, this));
		}, this)).finally(clearPasswords);
	},

	handleSave: function() {
		return this.saveForm().then(L.bind(function(config) {
			this.savedConfig = clone(config);
			ui.addNotification(null,
				E('p', {}, _('Configuration saved. Restart circud to apply it.')),
				'info');
		}, this)).catch(function(error) {
			ui.addNotification(_('Unable to save configuration'),
				E('p', {}, messageOf(error)), 'error');
			throw error;
		});
	},

	handleSaveApply: function() {
		const wasRunning = this.status.running === true;
		return this.saveForm().then(L.bind(function(config) {
			this.savedConfig = clone(config);
			if (!wasRunning)
				return null;
			return callServiceAction('restart');
		}, this)).then(L.bind(function(result) {
			if (result && result.success !== true)
				throw new Error(result.error || _('Service restart failed'));

			ui.addNotification(null, E('p', {}, wasRunning
				? _('Configuration saved and circud restarted.')
				: _('Configuration saved. circud remains stopped.')), 'info');
			return this.refreshStatus();
		}, this)).catch(function(error) {
			ui.addNotification(_('Unable to apply configuration'),
				E('p', {}, messageOf(error)), 'error');
			throw error;
		});
	},

	handleReset: function() {
		restoreFormData(this.formMap.data.data, this.savedConfig);
		return this.formMap.reset();
	}
});
