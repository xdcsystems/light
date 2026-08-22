'use strict';
'require view';
'require poll';
'require rpc';
'require ui';

const callGetStats = rpc.declare({
	object: 'light_control',
	method: 'get_stats'
});

const callGetStatus = rpc.declare({
	object: 'light_control',
	method: 'get_status'
});

const callGetConfig = rpc.declare({
	object: 'light_control',
	method: 'get_config'
});

const callReload = rpc.declare({
	object: 'light_control',
	method: 'reload'
});

const callSetBrightness = rpc.declare({
	object: 'light_control',
	method: 'set_brightness',
	params: [ 'value' ]
});

const callReleaseBrightness = rpc.declare({
	object: 'light_control',
	method: 'set_brightness',
	params: [ 'release' ]
});

function formatValue(value) {
	if (value === undefined || value === null) {
		return '-';
	}

	return String(value);
}

function formatTime(unixTime) {
	const sec = Number(unixTime);
	if (!sec) {
		return '-';
	}

	const date = new Date(sec * 1000);
	if (isNaN(date.getTime())) {
		return String(unixTime);
	}

	return date.toLocaleString();
}

function formatYesNo(value) {
	return value ? _('yes') : _('no');
}

function fetchData() {
	return Promise.all([
		L.resolveDefault(callGetStats(), null),
		L.resolveDefault(callGetStatus(), null),
		L.resolveDefault(callGetConfig(), null)
	]);
}

function renderStats(stats) {
	if (!stats) {
		return E('p', { 'class': 'alert-message warning' },
			_('Unable to read statistics. Is the light_control service running?'));
	}

	return E('table', { 'class': 'table' }, [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Metric')),
			E('th', { 'class': 'th' }, _('Value'))
		]),
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, _('Packets received')),
			E('td', { 'class': 'td left' }, formatValue(stats.packets_received))
		]),
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, _('Bytes received')),
			E('td', { 'class': 'td left' }, formatValue(stats.bytes_received))
		]),
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, _('Errors')),
			E('td', { 'class': 'td left' }, formatValue(stats.errors))
		])
	]);
}

function renderLastPacket(status) {
	if (!status) {
		return E('p', { 'class': 'alert-message warning' },
			_('Unable to read last packet status.'));
	}

	const rows = [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Field')),
			E('th', { 'class': 'th' }, _('Value'))
		]),
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left', 'width': '33%' }, _('Manual override')),
			E('td', { 'class': 'td left' }, formatYesNo(status.override))
		]),
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, _('Brightness (%)')),
			E('td', { 'class': 'td left' }, formatValue(status.brightness))
		])
	];

	if (!status.has_packet) {
		rows.push(E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, _('Last packet')),
			E('td', { 'class': 'td left' }, _('No valid sensor packet received yet.'))
		]));

		return E('table', { 'class': 'table' }, rows);
	}

	return E('table', { 'class': 'table' }, rows.concat([
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, _('Device ID')),
			E('td', { 'class': 'td left' }, formatValue(status.device_id))
		]),
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, _('Lux')),
			E('td', { 'class': 'td left' }, formatValue(status.lux))
		]),
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, _('Scene')),
			E('td', { 'class': 'td left' }, formatValue(status.scene || '-'))
		]),
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, _('Source')),
			E('td', { 'class': 'td left' }, formatValue(status.source_ip) + ':' +
				formatValue(status.source_port))
		]),
		E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, _('Last packet')),
			E('td', { 'class': 'td left' }, formatTime(status.unix_time))
		])
	]));
}

function renderMap(config) {
	const maps = config && Array.isArray(config.map) ? config.map : [];
	if (maps.length === 0) {
		return E('p', {}, _('No brightness map loaded.'));
	}

	const rows = [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Lux below')),
			E('th', { 'class': 'th' }, _('Brightness (%)')),
			E('th', { 'class': 'th' }, _('Set'))
		])
	];

	maps.forEach(function(entry) {
		rows.push(E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, formatValue(entry.lux_below)),
			E('td', { 'class': 'td left' }, formatValue(entry.brightness)),
			E('td', { 'class': 'td left' }, formatValue(entry.set || '-'))
		]));
	});

	return E('table', { 'class': 'table' }, rows);
}

function renderScenes(config) {
	const scenes = config && Array.isArray(config.scenes) ? config.scenes : [];
	if (scenes.length === 0) {
		return E('p', {}, _('No scenes loaded.'));
	}

	const rows = [
		E('tr', { 'class': 'tr table-titles' }, [
			E('th', { 'class': 'th' }, _('Name')),
			E('th', { 'class': 'th' }, _('From')),
			E('th', { 'class': 'th' }, _('To')),
			E('th', { 'class': 'th' }, _('Map set')),
			E('th', { 'class': 'th' }, _('Min')),
			E('th', { 'class': 'th' }, _('Max'))
		])
	];

	scenes.forEach(function(scene) {
		rows.push(E('tr', { 'class': 'tr' }, [
			E('td', { 'class': 'td left' }, formatValue(scene.name)),
			E('td', { 'class': 'td left' }, formatValue(scene.from_time)),
			E('td', { 'class': 'td left' }, formatValue(scene.to_time)),
			E('td', { 'class': 'td left' }, formatValue(scene.map_set || '-')),
			E('td', { 'class': 'td left' }, scene.min_brightness === undefined
				? '-' : formatValue(scene.min_brightness)),
			E('td', { 'class': 'td left' }, scene.max_brightness === undefined
				? '-' : formatValue(scene.max_brightness))
		]));
	});

	return E('table', { 'class': 'table' }, rows);
}

function renderLiveConfig(config) {
	if (!config) {
		return E('p', { 'class': 'alert-message warning' },
			_('Unable to read the live daemon config.'));
	}

	return E('div', {}, [
		E('table', { 'class': 'table' }, [
			E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left', 'width': '33%' }, _('UCI port / bound')),
				E('td', { 'class': 'td left' },
					formatValue(config.port) + ' / ' + formatValue(config.bound_port))
			]),
			E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left' }, _('Interface')),
				E('td', { 'class': 'td left' }, formatValue(config.interface))
			]),
			E('tr', { 'class': 'tr' }, [
				E('td', { 'class': 'td left' }, _('Enabled in UCI')),
				E('td', { 'class': 'td left' }, formatYesNo(config.enabled))
			])
		]),
		E('h4', {}, _('Brightness map')),
		renderMap(config),
		E('h4', {}, _('Scenes')),
		renderScenes(config)
	]);
}

function renderPolled(stats, status, config) {
	return E('div', {}, [
		E('h3', {}, _('Statistics')),
		renderStats(stats),
		E('h3', {}, _('Output')),
		renderLastPacket(status),
		E('h3', {}, _('Live config')),
		renderLiveConfig(config)
	]);
}

function readBrightnessInput() {
	const input = document.getElementById('light-control-brightness');
	if (!input) {
		return null;
	}

	const value = Number(input.value);
	if (!Number.isFinite(value) || value < 0 || value > 100) {
		return null;
	}

	return Math.round(value);
}

function refreshPolled() {
	return fetchData().then(function(next) {
		const node = document.getElementById('light-control-stats');
		if (!node) {
			return;
		}

		while (node.firstChild) {
			node.removeChild(node.firstChild);
		}

		node.appendChild(renderPolled(next[0], next[1], next[2]));
	});
}

return view.extend({
	load: function() {
		return fetchData();
	},

	handleSetBrightness: function() {
		const value = readBrightnessInput();
		if (value === null) {
			return ui.addNotification(null,
				E('p', {}, _('Brightness must be an integer from 0 to 100.')), 'error');
		}

		return callSetBrightness(value).then(refreshPolled);
	},

	handleRelease: function() {
		return callReleaseBrightness(true).then(refreshPolled);
	},

	handleReload: function() {
		return callReload().then(refreshPolled);
	},

	render: function(data) {
		const stats = data ? data[0] : null;
		const status = data ? data[1] : null;
		const config = data ? data[2] : null;
		const box = E('div', { 'id': 'light-control-stats' }, [
			renderPolled(stats, status, config)
		]);

		const brightness = E('input', {
			'id': 'light-control-brightness',
			'type': 'number',
			'min': '0',
			'max': '100',
			'step': '1',
			'value': status && status.brightness !== undefined ? String(status.brightness) : '0',
			'style': 'width: 6em'
		});

		poll.add(function() {
			return refreshPolled();
		}, 3);

		return E('div', {}, [
			E('h2', {}, _('Light Control')),
			E('div', { 'class': 'cbi-section-descr' },
				_('UDP packet statistics, last lux reading, live map/scenes, and a manual brightness hold.')),
			E('div', { 'class': 'cbi-section' }, [ box ]),
			E('h3', {}, _('Manual brightness')),
			E('div', { 'class': 'cbi-section-descr' },
				_('Holds the dimmer at this value until released. Sensor packets still update lux; output stays put.')),
			E('div', { 'class': 'cbi-section' }, [
				E('div', {}, [
					E('label', { 'for': 'light-control-brightness' }, _('Brightness (%)') + ' '),
					brightness
				]),
				E('div', { 'class': 'cbi-page-actions' }, [
					E('button', {
						'class': 'btn cbi-button cbi-button-save',
						'click': ui.createHandlerFn(this, 'handleSetBrightness')
					}, _('Hold')),
					' ',
					E('button', {
						'class': 'btn cbi-button cbi-button-reset',
						'click': ui.createHandlerFn(this, 'handleRelease')
					}, _('Release')),
					' ',
					E('button', {
						'class': 'btn cbi-button cbi-button-action',
						'click': ui.createHandlerFn(this, 'handleReload')
					}, _('Reload UCI'))
				])
			])
		]);
	},

	handleSaveApply: null,
	handleSave: null,
	handleReset: null
});
