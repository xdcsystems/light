'use strict';
'require view';
'require form';

return view.extend({
	render: function() {
		const m = new form.Map('light_control', _('Light Control'),
			_('Configure the light control daemon. Saving writes UCI and restarts the service. Map and scene changes can also be picked up from Status → Reload UCI without a process restart; a new UDP port still needs a restart.'));

		const s = m.section(form.TypedSection, 'light_control', _('Service'));
		s.anonymous = true;
		s.addremove = false;

		let o;

		o = s.option(form.Flag, 'enabled', _('Enable'),
			_('Start the light control daemon.'));
		o.default = '1';
		o.rmempty = false;

		o = s.option(form.Value, 'port', _('UDP port'),
			_('Port the daemon listens on for sensor packets.'));
		o.datatype = 'port';
		o.placeholder = '5005';
		o.rmempty = false;

		o = s.option(form.Value, 'interface', _('Listen interface'),
			_('Stored in UCI for later use. The daemon currently binds to 0.0.0.0.'));
		o.placeholder = 'lan';
		o.rmempty = true;

		const t = m.section(form.TableSection, 'map', _('Brightness map'),
			_('If measured lux is below a threshold, that brightness is used. Rows are matched from the lowest threshold to the highest.'));
		t.anonymous = true;
		t.addremove = true;

		o = t.option(form.Value, 'lux_below', _('Lux below'));
		o.datatype = 'range(1,65535)';
		o.placeholder = '200';
		o.rmempty = false;

		o = t.option(form.Value, 'brightness', _('Brightness (%)'));
		o.datatype = 'range(0,100)';
		o.placeholder = '100';
		o.rmempty = false;

		o = t.option(form.Value, 'set', _('Set name'),
			_('Optional group name. Empty rows are the default map. A scene can point at a named set via Map set.'));
		o.rmempty = true;

		const sc = m.section(form.TypedSection, 'scene', _('Scenes'),
			_('The local clock picks the first matching scene. Night may wrap past midnight (for example 18:00–06:00). Each scene uses the default map unless Map set names a group.'));
		sc.anonymous = false;
		sc.addremove = true;

		o = sc.option(form.Value, 'from_time', _('From'),
			_('Local time HH:MM, inclusive.'));
		o.placeholder = '06:00';
		o.rmempty = false;

		o = sc.option(form.Value, 'to_time', _('To'),
			_('Local time HH:MM, exclusive. Smaller than From means the interval wraps past midnight.'));
		o.placeholder = '11:00';
		o.rmempty = false;

		o = sc.option(form.Value, 'map_set', _('Map set'),
			_('Optional. Must match a Set name on map rows. Empty uses the default map.'));
		o.placeholder = 'daylight';
		o.rmempty = true;

		o = sc.option(form.Value, 'min_brightness', _('Min brightness (%)'),
			_('Optional clamp after the lux map is applied.'));
		o.datatype = 'range(0,100)';
		o.rmempty = true;

		o = sc.option(form.Value, 'max_brightness', _('Max brightness (%)'),
			_('Optional clamp after the lux map is applied.'));
		o.datatype = 'range(0,100)';
		o.rmempty = true;

		return m.render();
	}
});
